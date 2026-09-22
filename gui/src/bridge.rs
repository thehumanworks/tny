//! Thin, unprivileged process adapter: the CLI owns sessions, providers and tools.
use serde_json::Value;
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const MAX_JSON: usize = 16 * 1024 * 1024;
const MAX_EVENT: usize = 4 * 1024 * 1024;
const MAX_DIAGNOSTIC: usize = 16 * 1024;
// Local metadata must not pin a GUI worker on a stalled CLI or inherited pipe.
const METADATA_TIMEOUT: Duration = Duration::from_secs(12);

/// `None` means the CLI did not prove whether an artifact was committed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ImageFailure {
    pub path: Option<PathBuf>,
    pub committed: Option<bool>,
    pub error: String,
}

impl ImageFailure {
    fn unknown(error: String) -> Self {
        Self {
            path: None,
            committed: None,
            error,
        }
    }
}

#[derive(Clone)]
pub struct Bridge {
    binary: PathBuf,
    cwd: PathBuf,
    ssh: Option<String>,
    ssh_cwd: Option<String>,
}

impl Bridge {
    pub fn new(
        binary: PathBuf,
        cwd: PathBuf,
        ssh: Option<String>,
        ssh_cwd: Option<String>,
    ) -> Self {
        Self {
            binary,
            cwd,
            ssh,
            ssh_cwd,
        }
    }

    fn process(&self, remote_tools: bool) -> Result<Command, String> {
        // Arguments never pass through a local shell. tny owns remote quoting and SSH tools.
        let mut cmd = Command::new(&self.binary);
        cmd.current_dir(&self.cwd).arg("--cwd").arg(&self.cwd);
        if let Some(host) = self.ssh.as_ref().filter(|_| remote_tools) {
            if host.is_empty()
                || host.starts_with('-')
                || host.chars().any(|c| c.is_whitespace() || c.is_control())
            {
                return Err("Invalid SSH target".into());
            }
            cmd.arg("--ssh").arg(host);
        }
        if let Some(dir) = self.ssh_cwd.as_ref().filter(|_| remote_tools) {
            if self.ssh.is_none() || dir.is_empty() || dir.contains('\0') {
                return Err("Invalid remote working directory".into());
            }
            cmd.arg("--ssh-cwd").arg(dir);
        }
        Ok(cmd)
    }

    /// The GUI's fixed read/service operations, not a general-purpose argv gateway.
    pub fn command(&self, args: &[&str]) -> Result<Value, String> {
        let operation = match args {
            ["sessions", "--json"] => "sessions",
            ["usage", "--json"] => "usage",
            ["status", "--json"] => "status",
            ["agents", "--json"] => "agents",
            ["agents", "--run", run, "--json"] if hex_id(run, 32) => "agents",
            ["session", id, "--json"] if hex_id(id, 16) => "session",
            // `team status` takes a JSON request, not an ID in argv. These are the
            // only mailbox views exposed here. Inbox marks delivery, not ack.
            ["mailbox", "status", "--run", run, "--json"] if hex_id(run, 32) => "mailbox",
            ["mailbox", "inbox", "--run", run, "--json"] if hex_id(run, 32) => {
                // The CLI interprets inherited team identity as a worker. Never
                // strip those vars to impersonate a parent local operator.
                if inherited_team_identity() {
                    return Err("Parent mailbox inbox requires an unbound local operator".into());
                }
                "mailbox"
            }
            ["optimise", _, "--json"] => "optimise",
            ["dictate", "--seconds", "10", "--json"] => "dictate",
            _ => return Err("Unsupported GUI command or invalid identifier".into()),
        };
        let draft = if let ["optimise", draft, "--json"] = args {
            if draft.contains('\0') || draft.len() > 64 * 1024 {
                return Err("Invalid or oversized optimisation draft".into());
            }
            Some(*draft)
        } else {
            None
        };
        let mut cmd = self.process(operation == "optimise" || operation == "dictate")?;
        if draft.is_some() {
            cmd.args(["optimise", "--stdin", "--json"]);
            cmd.stdin(Stdio::piped());
        } else {
            cmd.args(args);
            cmd.stdin(Stdio::null());
        }
        let timeout = if draft.is_some() || operation == "dictate" {
            None
        } else {
            Some(METADATA_TIMEOUT)
        };
        run_json(cmd, operation, draft, timeout)
    }

    /// Inspect a local DAG run without losing the failed/interrupted job snapshot.
    /// Unlike `agents --run`, `jobs status` returns the job document even at rc=2.
    pub fn run_status(&self, run: &str) -> Result<Value, String> {
        if !lowercase_hex_id(run, 32) {
            return Err("Invalid run ID".into());
        }
        let mut cmd = self.process(false)?;
        cmd.args(["jobs", "status", run, "--json"])
            .stdin(Stdio::null());
        let result = run_process(cmd, "jobs status", None, Some(METADATA_TIMEOUT))?;
        let code = result.status.code();
        if code != Some(0) && code != Some(2) {
            return Err(exit_error("jobs status", result.status, &result.diagnostic));
        }
        let doc: Value = serde_json::from_slice(&result.stdout)
            .map_err(|_| "tny jobs status produced invalid JSON".to_string())?;
        if doc.get("kind").and_then(Value::as_str) != Some("job")
            || doc.get("id").and_then(Value::as_str) != Some(run)
            || doc.get("dag").and_then(Value::as_bool) != Some(true)
        {
            return Err("tny jobs status produced invalid run document".into());
        }
        let state = doc.get("state").and_then(Value::as_str);
        if !matches!(
            state,
            Some("queued" | "running" | "succeeded" | "failed" | "cancelled" | "interrupted")
        ) || (code == Some(2)) != matches!(state, Some("failed" | "interrupted"))
        {
            return Err("tny jobs status produced inconsistent run state".into());
        }
        Ok(doc)
    }

    /// Generate via the standalone image CLI. The CLI owns provider selection,
    /// output validation, manifest creation and atomic replacement.
    pub fn generate_image(&self, prompt: &str, output: &Path) -> Result<Value, ImageFailure> {
        if self.ssh.is_some() {
            return Err(ImageFailure::unknown(
                "Image generation requires a local workspace".into(),
            ));
        }
        if prompt.trim().is_empty() || prompt.contains('\0') || prompt.len() > 16 * 1024 {
            return Err(ImageFailure::unknown(
                "Invalid or oversized image prompt".into(),
            ));
        }
        if !output.is_absolute() || output.file_name().is_none() || output.to_str().is_none() {
            return Err(ImageFailure::unknown(
                "Image output must be an absolute local file path".into(),
            ));
        }
        let mut cmd = self.process(false).map_err(ImageFailure::unknown)?;
        cmd.args(["image", "generate", "--output-file"])
            .arg(output)
            .arg("--json")
            .stdin(Stdio::piped());
        let result = run_process(cmd, "image generate", Some(prompt), None)
            .map_err(ImageFailure::unknown)?;
        // Even with a nonzero exit, tny may have atomically committed the image
        // before its manifest finalization failed. Never discard that receipt.
        let doc: Value = serde_json::from_slice(&result.stdout).unwrap_or(Value::Null);
        if !result.status.success() {
            return Err(image_failure(
                output,
                result.status,
                &result.diagnostic,
                &doc,
            ));
        }
        if matches!(result.write_result, Some(Err(_))) {
            return Err(ImageFailure::unknown(
                "Could not deliver image prompt to tny".into(),
            ));
        }
        if doc.get("kind").and_then(Value::as_str) != Some("image")
            || doc.get("ok").and_then(Value::as_bool) != Some(true)
            || doc.get("operation").and_then(Value::as_str) != Some("generate")
            || doc.get("path").and_then(Value::as_str) != output.to_str()
        {
            return Err(ImageFailure::unknown(
                "tny image generate produced invalid result".into(),
            ));
        }
        Ok(doc)
    }

    fn saved_session_config(&self, id: &str) -> Result<(String, Option<String>), String> {
        let doc = self.command(&["session", id, "--json"])?;
        if doc.get("id").and_then(Value::as_str) != Some(id) {
            return Err("Saved session identity mismatch".into());
        }
        // Legacy sessions without a provider use openai; missing model falls
        // back to that provider's current default, as in the sessions contract.
        let provider = match doc.get("backend") {
            None | Some(Value::Null) => "openai",
            Some(Value::String(name)) if valid_provider(name) => name,
            _ => return Err("Invalid saved session provider".into()),
        };
        let model = match doc.get("model") {
            None | Some(Value::Null) => None,
            Some(Value::String(name)) if valid_model(name) => Some(name.clone()),
            _ => return Err("Invalid saved session model".into()),
        };
        Ok((provider.to_owned(), model))
    }

    pub fn ask_stream(
        &self,
        prompt: &str,
        resume: Option<&str>,
        swarm: Option<u8>,
        on_event: impl FnMut(Value),
    ) -> Result<(), String> {
        self.ask_stream_image(prompt, resume, swarm, None, on_event)
    }

    pub fn ask_stream_image(
        &self,
        prompt: &str,
        resume: Option<&str>,
        swarm: Option<u8>,
        image: Option<&str>,
        mut on_event: impl FnMut(Value),
    ) -> Result<(), String> {
        if resume.is_some_and(|id| !hex_id(id, 16)) {
            return Err("Invalid session ID".into());
        }
        if let Some(count) = swarm {
            if !(1..=16).contains(&count) || self.ssh.is_some() {
                return Err("Swarm requires 1..16 local agents".into());
            }
        }
        if prompt.trim().is_empty() {
            return Err("Enter a prompt before starting a turn".into());
        }
        if let Some(path) = image {
            if path.trim().is_empty() || path.contains('\0') {
                return Err("Image path must name a local regular file".into());
            }
            // --ssh moves tool execution, not this CLI process. `ask --image`
            // loads a local path; resolve relative names against the selected cwd.
            let local = Path::new(path);
            let local = if local.is_absolute() {
                local.to_path_buf()
            } else {
                self.cwd.join(local)
            };
            if !local.is_file() {
                return Err("Image path must name a local regular file".into());
            }
        }
        // A fresh `tny ask` process resolves the current default before it opens
        // --resume. Pin the saved session's provider/model in leading CLI flags,
        // rather than accidentally continuing a transcript on a new default.
        let saved_config = resume.map(|id| self.saved_session_config(id)).transpose()?;
        let mut cmd = self.process(true)?;
        if let Some((provider, model)) = saved_config.as_ref() {
            cmd.arg("--provider").arg(provider);
            if let Some(model) = model {
                cmd.arg("--model").arg(model);
            }
        }
        cmd.args(["ask", "--events=jsonl", "--progress=none", "--stdin"]);
        if let Some(id) = resume {
            cmd.args(["--resume", id]);
        }
        if let Some(count) = swarm {
            cmd.arg(format!("--swarm={count}"));
        }
        if let Some(path) = image {
            cmd.arg("--image").arg(path);
        }
        cmd.stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = spawn_tny(&mut cmd).map_err(|_| "Could not start tny ask".to_string())?;
        let stderr = drain_stderr(&mut child);
        // Write concurrently with stdout consumption, avoiding pipe deadlocks.
        let writer = write_stdin(child.stdin.take().expect("piped stdin"), prompt);
        let mut output = BufReader::new(child.stdout.take().expect("piped stdout"));
        let mut terminal = None;
        let stream = (|| -> Result<(), String> {
            while let Some(line) = read_line(&mut output, MAX_EVENT)
                .map_err(|_| "Invalid or oversized tny event stream".to_string())?
            {
                let event: Value = serde_json::from_slice(&line)
                    .map_err(|_| "Invalid tny event JSON".to_string())?;
                let kind = event.get("type").and_then(Value::as_str);
                if !event.is_object() || kind.is_none() || terminal.is_some() {
                    return Err("Invalid tny event order".into());
                }
                if kind == Some("turn_end") {
                    terminal = Some(
                        event
                            .get("stop_reason")
                            .and_then(Value::as_u64)
                            .ok_or("Invalid tny turn end")?,
                    );
                }
                on_event(event);
            }
            Ok(())
        })();
        if stream.is_err() {
            let _ = child.kill();
        }
        let status = child
            .wait()
            .map_err(|_| "Could not wait for tny ask".to_string())?;
        let write_result = writer
            .recv_timeout(Duration::from_secs(1))
            .unwrap_or_else(|_| Err(io::Error::other("prompt writer failed")));
        let diagnostic = stderr
            .recv_timeout(Duration::from_secs(1))
            .unwrap_or_default();
        stream?;
        if !status.success() {
            return Err(exit_error("ask", status, &diagnostic));
        }
        if write_result.is_err() {
            return Err("Could not deliver prompt to tny".into());
        }
        match terminal {
            Some(0) => Ok(()),
            Some(_) => Err("tny turn stopped without success".into()),
            None => Err("tny event stream ended without a turn_end".into()),
        }
    }
}

struct ProcessResult {
    status: ExitStatus,
    stdout: Vec<u8>,
    diagnostic: Vec<u8>,
    write_result: Option<io::Result<()>>,
}

fn run_json(
    cmd: Command,
    operation: &'static str,
    stdin: Option<&str>,
    timeout: Option<Duration>,
) -> Result<Value, String> {
    let result = run_process(cmd, operation, stdin, timeout)?;
    if !result.status.success() {
        return Err(exit_error(operation, result.status, &result.diagnostic));
    }
    if matches!(result.write_result, Some(Err(_))) {
        return Err(format!("Could not deliver input to tny {operation}"));
    }
    let value: Value = serde_json::from_slice(&result.stdout)
        .map_err(|_| format!("tny {operation} produced invalid JSON"))?;
    if !value.is_object() {
        return Err(format!("tny {operation} produced invalid JSON"));
    }
    Ok(value)
}

fn run_process(
    mut cmd: Command,
    operation: &'static str,
    stdin: Option<&str>,
    timeout: Option<Duration>,
) -> Result<ProcessResult, String> {
    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = spawn_tny(&mut cmd).map_err(|_| format!("Could not start tny {operation}"))?;
    let stderr_rx = drain_stderr(&mut child);
    let stdout = child.stdout.take().expect("piped stdout");
    let (stdout_tx, stdout_rx) = mpsc::sync_channel(1);
    thread::spawn(move || {
        let _ = stdout_tx.send(read_bounded(stdout, MAX_JSON));
    });
    let writer_rx = stdin.map(|text| write_stdin(child.stdin.take().expect("piped stdin"), text));
    let started = Instant::now();
    let mut status = None;
    let mut bytes = None;
    let mut diagnostic = None;
    let mut write_result = None;
    loop {
        if status.is_none() {
            status = match child.try_wait() {
                Ok(status) => status,
                Err(_) => {
                    let _ = child.kill();
                    let _ = child.wait();
                    return Err(format!("Could not wait for tny {operation}"));
                }
            };
        }
        if bytes.is_none() {
            match stdout_rx.try_recv() {
                Ok(Ok(data)) => bytes = Some(data),
                Ok(Err(_)) | Err(mpsc::TryRecvError::Disconnected) => {
                    if status.is_none() {
                        let _ = child.kill();
                        let _ = child.wait();
                    }
                    return Err(format!(
                        "tny {operation} produced oversized or unreadable JSON"
                    ));
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        if diagnostic.is_none() {
            match stderr_rx.try_recv() {
                Ok(data) => diagnostic = Some(data),
                Err(mpsc::TryRecvError::Disconnected) => diagnostic = Some(Vec::new()),
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        if let Some(rx) = writer_rx.as_ref() {
            if write_result.is_none() {
                match rx.try_recv() {
                    Ok(result) => write_result = Some(result),
                    Err(mpsc::TryRecvError::Disconnected) => {
                        write_result = Some(Err(io::Error::other("stdin writer failed")));
                    }
                    Err(mpsc::TryRecvError::Empty) => {}
                }
            }
        }
        if status.is_some()
            && bytes.is_some()
            && diagnostic.is_some()
            && (writer_rx.is_none() || write_result.is_some())
        {
            return Ok(ProcessResult {
                status: status.expect("checked status"),
                stdout: bytes.expect("checked stdout"),
                diagnostic: diagnostic.expect("checked stderr"),
                write_result,
            });
        }
        if timeout.is_some_and(|limit| started.elapsed() >= limit) {
            if status.is_none() {
                let _ = child.kill();
                let _ = child.wait();
            }
            // An exited child can leave an inherited pipe in a descendant.
            // Never join a pipe reader here; its bounded buffer is discarded on EOF.
            return Err(format!("tny {operation} timed out"));
        }
        thread::sleep(Duration::from_millis(10));
    }
}

fn write_stdin(
    mut input: impl Write + Send + 'static,
    text: &str,
) -> mpsc::Receiver<io::Result<()>> {
    let bytes = text.as_bytes().to_vec();
    let (tx, rx) = mpsc::sync_channel(1);
    thread::spawn(move || {
        let _ = tx.send(input.write_all(&bytes));
    });
    rx
}

fn spawn_tny(cmd: &mut Command) -> io::Result<Child> {
    // A GUI may launch a local binary while it is being replaced by a build.
    // Retrying ETXTBSY is safe: exec failed, so no first process ran.
    for attempt in 0..3 {
        match cmd.spawn() {
            Err(err) if err.kind() == io::ErrorKind::ExecutableFileBusy && attempt < 2 => {
                thread::sleep(Duration::from_millis(10));
            }
            result => return result,
        }
    }
    unreachable!()
}

fn hex_id(id: &str, len: usize) -> bool {
    id.len() == len && id.bytes().all(|b| b.is_ascii_hexdigit())
}

fn inherited_team_identity() -> bool {
    std::env::var_os("TNY_NESTED").is_some()
        || std::env::vars_os().any(|(name, _)| name.to_string_lossy().starts_with("TNY_TEAM_"))
}

fn lowercase_hex_id(id: &str, len: usize) -> bool {
    id.len() == len
        && id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn valid_provider(name: &str) -> bool {
    !name.is_empty()
        && !name.starts_with('-')
        && name.len() <= 256
        && !name.chars().any(|c| c.is_whitespace() || c.is_control())
}

fn valid_model(model: &str) -> bool {
    !model.is_empty() && model.len() <= 1024 && !model.chars().any(char::is_control)
}

fn read_bounded(mut input: impl Read, max: usize) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    let mut chunk = [0; 8192];
    loop {
        let n = input.read(&mut chunk)?;
        if n == 0 {
            return Ok(bytes);
        }
        if n > max - bytes.len() {
            return Err(io::Error::other("output limit"));
        }
        bytes.extend_from_slice(&chunk[..n]);
    }
}

// BufRead::read_until is unbounded on a missing newline. Bound each frame before parsing.
fn read_line(input: &mut impl BufRead, max: usize) -> io::Result<Option<Vec<u8>>> {
    let mut line = Vec::new();
    loop {
        let available = input.fill_buf()?;
        if available.is_empty() {
            return if line.is_empty() {
                Ok(None)
            } else {
                Ok(Some(line))
            };
        }
        let count = available
            .iter()
            .position(|b| *b == b'\n')
            .map_or(available.len(), |p| p + 1);
        if count > max - line.len() {
            return Err(io::Error::other("line limit"));
        }
        line.extend_from_slice(&available[..count]);
        input.consume(count);
        if line.last() == Some(&b'\n') {
            return Ok(Some(line));
        }
    }
}

fn drain_stderr(child: &mut Child) -> mpsc::Receiver<Vec<u8>> {
    let mut pipe = child.stderr.take().expect("piped stderr");
    let (tx, rx) = mpsc::sync_channel(1);
    thread::spawn(move || {
        let mut diagnostic = Vec::new();
        let mut chunk = [0; 4096];
        while let Ok(n) = pipe.read(&mut chunk) {
            if n == 0 {
                break;
            }
            let keep = n.min(MAX_DIAGNOSTIC - diagnostic.len());
            diagnostic.extend_from_slice(&chunk[..keep]);
        }
        let _ = tx.send(diagnostic);
    });
    rx
}

fn image_failure(
    output: &Path,
    status: ExitStatus,
    diagnostic: &[u8],
    doc: &Value,
) -> ImageFailure {
    let mut failure = ImageFailure::unknown(exit_error("image generate", status, diagnostic));
    if doc.get("kind").and_then(Value::as_str) != Some("image")
        || doc.get("ok").and_then(Value::as_bool) != Some(false)
        || doc.get("operation").and_then(Value::as_str) != Some("generate")
    {
        return failure;
    }
    let code = doc.get("code").and_then(Value::as_str);
    if code == Some("IMAGE_MANIFEST_FINALIZE_FAILED")
        && doc.get("committed").and_then(Value::as_bool) == Some(true)
        && doc.get("path").and_then(Value::as_str) == output.to_str()
    {
        failure.path = Some(output.to_path_buf());
        failure.committed = Some(true);
        failure.error = "Image saved, but its manifest could not be finalized".into();
    } else if matches!(
        code,
        Some(
            "IMAGE_STRICT_SIZE_INVALID"
                | "IMAGE_SIZE_MISMATCH"
                | "IMAGE_SIZE_UNVERIFIABLE"
                | "IMAGE_SIZE_UNSUPPORTED"
        )
    ) && doc.get("committed").and_then(Value::as_bool) == Some(false)
        && doc.get("path").is_some_and(Value::is_null)
    {
        failure.committed = Some(false);
    }
    // No arbitrary `error`, path, code or stderr string reaches the GUI.
    failure
}

fn exit_error(operation: &str, status: ExitStatus, diagnostic: &[u8]) -> String {
    // Never echo untrusted stderr or CLI JSON `message`: either can contain keys,
    // provider responses, paths or the user's prompt. Retain only a known error code.
    let code = diagnostic.split(|b| *b == b'\n').next().and_then(|first| {
        let doc: Value = serde_json::from_slice(first).ok()?;
        if doc.get("kind")?.as_str()? != "ask_error" {
            return None;
        }
        match doc.get("code")?.as_str()? {
            "invalid_option" => Some("invalid_option"),
            "option_conflict" => Some("option_conflict"),
            "no_prompt" => Some("no_prompt"),
            "session" => Some("session"),
            "session_busy" => Some("session_busy"),
            "provider" => Some("provider"),
            "internal" => Some("internal"),
            "start_failed" => Some("start_failed"),
            "stream_io" => Some("stream_io"),
            "no_terminal" => Some("no_terminal"),
            "cancelled" => Some("cancelled"),
            _ => None,
        }
    });
    let status = status
        .code()
        .map_or("signal".to_string(), |n| n.to_string());
    match code {
        Some(code) => format!("tny {operation} failed (exit {status}, {code})"),
        None => format!("tny {operation} failed (exit {status})"),
    }
}

#[cfg(test)]
include!("bridge_tests.rs");
