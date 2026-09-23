//! Native desktop shell. CLI processes run on workers; only UI model updates run on Slint's loop.
mod bridge;

use bridge::{parse_providers, Bridge, ImageFailure, ModelChoice, ModelInfo, ProviderInfo};
use serde_json::Value;
use slint::{Model, ModelRc, SharedString, VecModel, Weak};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::rc::Rc;
use std::sync::mpsc;
use std::thread;
use std::time::Duration;
use std::time::Instant;

slint::include_modules!();

struct State {
    bridge: Bridge,
    binary: PathBuf,
    cwd: PathBuf,
    ssh: Option<String>,
    session_id: Option<String>,
    session_usage: Option<String>,
    inbox_in_flight: bool,
    parent_inbox_authorized: bool,
    generation: u64,
    refresh_serial: u64,
    refresh_gate: RefreshGate,
    index_gate: RefreshGate,
    allowance_in_flight: bool,
    busy: bool,
    swarm: Option<u8>,
    swarm_run: Option<String>,
    rows: Rc<VecModel<ChatRow>>,
    sessions: Vec<SessionRow>,
    current_response: Option<usize>,
    turn_has_error: bool,
    unconfirmed: bool,
    requires_tool_confirmation: bool,
    saved_read_only: bool,
    image_gen_busy: bool,
    generated_path: Option<PathBuf>,
    local_index: LocalIndex,
    completion_source: String,
    completions: Vec<Completion>,
    picker: Picker,
}

/// Provider/model picker. `choice` is what the next turn passes as leading
/// `--provider/--model`; `None` leaves resolution to the CLI (and, for a
/// resumed session, to the bridge's saved-session pin).
#[derive(Default)]
struct Picker {
    providers: Vec<ProviderInfo>,
    providers_loaded: bool,
    providers_in_flight: bool,
    /// The picker was opened before the provider list arrived.
    models_wanted: bool,
    choice: Option<ModelChoice>,
    catalogs: HashMap<String, Vec<ModelInfo>>,
    catalog_failed: HashSet<String>,
    catalog_in_flight: HashSet<String>,
}

impl Picker {
    /// The provider the next turn will use, when it is known.
    fn provider(&self) -> Option<&str> {
        self.choice
            .as_ref()
            .map(|c| c.provider.as_str())
            .or_else(|| {
                self.providers
                    .iter()
                    .find(|p| p.active)
                    .map(|p| p.name.as_str())
            })
    }

    fn model(&self) -> Option<&str> {
        self.choice.as_ref().and_then(|c| c.model.as_deref())
    }

    fn label(&self) -> String {
        let Some(provider) = self.provider() else {
            return "Default model".into();
        };
        let model = match self.model() {
            None => "Default".to_string(),
            Some(id) => self
                .catalogs
                .get(provider)
                .and_then(|list| list.iter().find(|m| m.id == id))
                .map_or_else(|| id.to_string(), |m| m.name.clone()),
        };
        format!("{provider} · {model}")
    }

    fn status(&self) -> &'static str {
        let Some(provider) = self.provider() else {
            return if self.providers_loaded {
                "No provider is configured. Set one up with tny setup in a terminal."
            } else {
                "Loading providers…"
            };
        };
        if self.catalog_in_flight.contains(provider) {
            "Loading models…"
        } else if self.catalog_failed.contains(provider) {
            "Couldn't load this provider's model list. Default still works, or enter a model ID below."
        } else if self.catalogs.get(provider).is_some_and(Vec::is_empty) {
            "This provider doesn't publish a model list. Use Default or enter a model ID below."
        } else {
            ""
        }
    }
}

/// A saved session's provider/model, as the CLI would resume it: a legacy
/// session without a provider is `openai`, a missing model is that provider's default.
fn saved_choice(doc: &Value) -> Option<ModelChoice> {
    let provider = match doc.get("backend") {
        None | Some(Value::Null) => "openai",
        Some(Value::String(name)) => name,
        _ => return None,
    };
    let model = match doc.get("model") {
        None | Some(Value::Null) => None,
        Some(Value::String(name)) => Some(name.clone()),
        _ => return None,
    };
    let choice = ModelChoice {
        provider: provider.into(),
        model,
    };
    choice.is_valid().then_some(choice)
}

fn provider_rows(picker: &Picker) -> Vec<ProviderRow> {
    picker
        .providers
        .iter()
        .map(|p| ProviderRow {
            name: p.name.clone().into(),
            detail: match (p.ready, p.active) {
                (true, true) => "Default · ready",
                (true, false) => "Ready",
                (false, _) => "Needs setup",
            }
            .into(),
            ready: p.ready,
        })
        .collect()
}

fn model_rows(picker: &Picker) -> Vec<ModelRow> {
    let listed = picker
        .provider()
        .and_then(|p| picker.catalogs.get(p))
        .map(Vec::as_slice)
        .unwrap_or_default();
    let mut rows: Vec<ModelRow> = listed
        .iter()
        .map(|m| ModelRow {
            id: m.id.clone().into(),
            label: m.name.clone().into(),
            detail: if !m.description.is_empty() {
                m.description.clone()
            } else if m.name != m.id {
                m.id.clone()
            } else {
                String::new()
            }
            .into(),
        })
        .collect();
    // A typed or saved model that the catalog does not list still shows as selected.
    if let Some(id) = picker.model() {
        if !listed.iter().any(|m| m.id == id) {
            rows.insert(
                0,
                ModelRow {
                    id: id.into(),
                    label: id.into(),
                    detail: "Not in this provider's list".into(),
                },
            );
        }
    }
    rows
}

fn render_picker(ui: &App, picker: &Picker) {
    ui.set_providers(ModelRc::from(Rc::new(VecModel::from(provider_rows(
        picker,
    )))));
    ui.set_models(ModelRc::from(Rc::new(VecModel::from(model_rows(picker)))));
    ui.set_picker_provider(picker.provider().unwrap_or("").into());
    ui.set_picker_model(picker.model().unwrap_or("").into());
    ui.set_model_label(picker.label().into());
    ui.set_models_status(picker.status().into());
}

fn load_providers(ui: &App, state: &Rc<RefCell<State>>) {
    let bridge = {
        let mut s = state.borrow_mut();
        if s.picker.providers_in_flight {
            return;
        }
        s.picker.providers_in_flight = true;
        s.bridge.clone()
    };
    // Local configuration only: `providers` resolves credentials, never inference.
    dispatch(
        ui.as_weak(),
        move || {
            bridge
                .command(&["providers", "--json"])
                .and_then(|doc| parse_providers(&doc))
        },
        move |ui, result| {
            let Some(state) = ui_state(ui) else {
                return;
            };
            let wanted = {
                let mut s = state.borrow_mut();
                s.picker.providers_in_flight = false;
                match result {
                    Ok(list) => {
                        s.picker.providers = list;
                        s.picker.providers_loaded = true;
                    }
                    Err(_) => app_error(ui, "Provider list"),
                }
                render_picker(ui, &s.picker);
                std::mem::take(&mut s.picker.models_wanted)
            };
            if wanted {
                load_models(ui, &state);
            }
        },
    );
}

/// Model catalogs are provider requests: fetched only when the picker needs
/// them, once per provider per window (Refresh clears the cache).
fn load_models(ui: &App, state: &Rc<RefCell<State>>) {
    let (bridge, provider) = {
        let mut s = state.borrow_mut();
        let Some(provider) = s.picker.provider().map(str::to_owned) else {
            return;
        };
        if s.picker.catalogs.contains_key(&provider)
            || s.picker.catalog_failed.contains(&provider)
            || !s.picker.catalog_in_flight.insert(provider.clone())
        {
            return;
        }
        (s.bridge.clone(), provider)
    };
    render_picker(ui, &state.borrow().picker);
    let requested = provider.clone();
    dispatch(
        ui.as_weak(),
        move || bridge.models(&requested),
        move |ui, result| {
            let Some(state) = ui_state(ui) else {
                return;
            };
            let mut s = state.borrow_mut();
            s.picker.catalog_in_flight.remove(&provider);
            match result {
                Ok(list) => {
                    s.picker.catalogs.insert(provider, list);
                }
                Err(_) => {
                    s.picker.catalog_failed.insert(provider);
                }
            }
            render_picker(ui, &s.picker);
        },
    );
}

#[derive(Default, Debug)]
struct RefreshGate {
    in_flight: bool,
    pending: bool,
}

impl RefreshGate {
    fn request(&mut self) -> bool {
        if self.in_flight {
            self.pending = true;
            false
        } else {
            self.in_flight = true;
            true
        }
    }

    fn complete(&mut self) -> bool {
        self.in_flight = false;
        std::mem::take(&mut self.pending)
    }
}

const INDEX_MAX_ENTRIES: usize = 2048;
const INDEX_MAX_DIRS: usize = 96;
const INDEX_MAX_DEPTH: usize = 4;
const INDEX_MAX_FILES: usize = 512;
const INDEX_MAX_SKILLS: usize = 128;
const COMPLETION_MAX: usize = 8;
const GUI_COMMANDS: &[&str] = &["/new", "/refresh", "/usage", "/help"];
const SKILL_DIRS: &[&str] = &[
    "skills",
    ".agents/skills",
    ".claude/skills",
    ".codex/skills",
    ".cursor/skills",
    ".opencode/skills",
];

#[derive(Default)]
struct LocalIndex {
    paths: Vec<String>,
    skills: Vec<String>,
    ready: bool,
    paths_unavailable: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CompletionKind {
    File,
    Skill,
    Slash,
}

#[derive(Clone, Debug)]
struct Completion {
    kind: CompletionKind,
    label: String,
    start: usize,
    replacement: String,
    query: String,
}

fn has_no_symlink_components(path: &Path) -> bool {
    let mut count = 0;
    for part in path.ancestors() {
        count += 1;
        if count > 64
            || !fs::symlink_metadata(part).is_ok_and(|meta| !meta.file_type().is_symlink())
        {
            return false;
        }
    }
    true
}

fn scan_non_git_paths(cwd: &Path) -> Vec<String> {
    // All directory enumeration happens on a worker. No symlinked root,
    // descendant directory or file is ever intentionally followed.
    if !has_no_symlink_components(cwd)
        || !fs::symlink_metadata(cwd).is_ok_and(|m| m.is_dir() && !m.file_type().is_symlink())
    {
        return Vec::new();
    }
    let mut stack = vec![(cwd.to_path_buf(), 0usize)];
    let (mut visited, mut dirs) = (0usize, 0usize);
    let mut files = Vec::new();
    while let Some((dir, depth)) = stack.pop() {
        if dirs >= INDEX_MAX_DIRS || visited >= INDEX_MAX_ENTRIES || files.len() >= INDEX_MAX_FILES
        {
            break;
        }
        dirs += 1;
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.take(256).flatten() {
            visited += 1;
            if visited > INDEX_MAX_ENTRIES {
                break;
            }
            let name = entry.file_name();
            if name == ".git" || name == "target" {
                continue;
            }
            let Ok(ty) = entry.file_type() else {
                continue;
            };
            if ty.is_symlink() {
                continue;
            }
            let path = entry.path();
            if ty.is_dir() {
                if files.len() < INDEX_MAX_FILES {
                    if let Ok(relative) = path.strip_prefix(cwd) {
                        if let Some(name) = relative.to_str() {
                            if name.len() <= 179
                                && !name.contains('`')
                                && !name.chars().any(char::is_control)
                            {
                                files.push(format!("{name}/"));
                            }
                        }
                    }
                }
                if depth < INDEX_MAX_DEPTH && dirs + stack.len() < INDEX_MAX_DIRS {
                    stack.push((path, depth + 1));
                }
            } else if ty.is_file() && files.len() < INDEX_MAX_FILES {
                let Ok(relative) = path.strip_prefix(cwd) else {
                    continue;
                };
                let Some(name) = relative.to_str() else {
                    continue;
                };
                if name.len() <= 180 && !name.contains('`') && !name.chars().any(char::is_control) {
                    files.push(name.to_owned());
                }
            }
        }
    }
    files.sort_unstable();
    files
}

const GIT_OUTPUT_LIMIT: u64 = 512 * 1024;
const GIT_TIMEOUT: Duration = Duration::from_secs(4);

fn git_output(cwd: &Path, args: &[&str]) -> Result<(ExitStatus, Vec<u8>), ()> {
    let mut child = Command::new("git")
        .current_dir(cwd)
        .args(args)
        .env_remove("GIT_DIR")
        .env_remove("GIT_WORK_TREE")
        .env_remove("GIT_INDEX_FILE")
        .env("GIT_OPTIONAL_LOCKS", "0")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| ())?;
    let pipe = child.stdout.take().ok_or(())?;
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let mut output = Vec::new();
        let result = pipe.take(GIT_OUTPUT_LIMIT + 1).read_to_end(&mut output);
        let _ = sender.send(result.map(|_| output));
    });
    let deadline = Instant::now() + GIT_TIMEOUT;
    loop {
        if let Ok(Some(status)) = child.try_wait() {
            let bytes = receiver
                .recv_timeout(deadline.saturating_duration_since(Instant::now()))
                .map_err(|_| ())?
                .map_err(|_| ())?;
            return if bytes.len() as u64 > GIT_OUTPUT_LIMIT {
                Err(())
            } else {
                Ok((status, bytes))
            };
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            return Err(());
        }
        thread::sleep(Duration::from_millis(20));
    }
}

fn maybe_git_marker(cwd: &Path) -> bool {
    let mut count = 0;
    for ancestor in cwd.ancestors() {
        count += 1;
        if count > 64 {
            return true;
        }
        match fs::symlink_metadata(ancestor.join(".git")) {
            Ok(_) => return true,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(_) => return true, // unknown authority: fail closed
        }
    }
    false
}

fn git_paths(cwd: &Path, output: &[u8]) -> Vec<String> {
    let mut paths = HashSet::new();
    for raw in output
        .split(|b| *b == 0)
        .filter(|raw| !raw.is_empty())
        .take(INDEX_MAX_ENTRIES)
    {
        if paths.len() >= INDEX_MAX_FILES {
            break;
        }
        let Ok(text) = std::str::from_utf8(raw) else {
            continue;
        };
        let relative = Path::new(text);
        if relative.is_absolute()
            || relative.components().any(|component| match component {
                std::path::Component::Normal(name) => name == ".git" || name == "target",
                _ => true,
            })
        {
            continue;
        }
        let components = relative.components().count();
        if components == 0
            || components > INDEX_MAX_DEPTH + 1
            || text.len() > 180
            || text.contains('`')
            || text.chars().any(char::is_control)
        {
            continue;
        }
        let path = cwd.join(relative);
        if !has_no_symlink_components(&path) {
            continue;
        }
        if !fs::symlink_metadata(&path).is_ok_and(|m| m.is_file() && !m.file_type().is_symlink()) {
            continue;
        }
        paths.insert(text.to_owned());
        let mut parent = relative.parent();
        while let Some(dir) = parent {
            if dir.as_os_str().is_empty() || paths.len() >= INDEX_MAX_FILES {
                break;
            }
            if let Some(dir) = dir.to_str() {
                paths.insert(format!("{dir}/"));
            }
            parent = dir.parent();
        }
    }
    let mut paths: Vec<_> = paths.into_iter().collect();
    paths.sort_unstable();
    paths
}

// A git command failure in a possible repository is not permission to list
// ignored/private paths. Only a proven non-repository uses the bounded scan.
fn index_paths(cwd: &Path) -> (Vec<String>, bool) {
    if !has_no_symlink_components(cwd) {
        return (Vec::new(), true);
    }
    match git_output(cwd, &["rev-parse", "--is-inside-work-tree"]) {
        Ok((status, output)) if status.success() && output == b"true\n" => {
            match git_output(
                cwd,
                &[
                    "ls-files",
                    "--cached",
                    "--others",
                    "--exclude-standard",
                    "-z",
                    "--",
                    ".",
                ],
            ) {
                Ok((status, output)) if status.success() => (git_paths(cwd, &output), false),
                _ => (Vec::new(), true),
            }
        }
        Ok((status, _)) if status.code() == Some(128) && !maybe_git_marker(cwd) => {
            (scan_non_git_paths(cwd), false)
        }
        _ => (Vec::new(), true),
    }
}

fn skill_name(path: &Path) -> Option<String> {
    // Metadata and bounded read protect against symlinks, FIFOs and huge skills.
    let meta = fs::symlink_metadata(path).ok()?;
    if !meta.is_file() || meta.file_type().is_symlink() {
        return None;
    }
    let mut bytes = Vec::new();
    fs::File::open(path)
        .ok()?
        .take(4096)
        .read_to_end(&mut bytes)
        .ok()?;
    let text = std::str::from_utf8(&bytes).ok()?;
    let mut lines = text.lines();
    if lines.next()? != "---" {
        return None;
    }
    for line in lines {
        if line == "---" {
            break;
        }
        let Some(name) = line.strip_prefix("name:") else {
            continue;
        };
        let name = name.trim();
        if !name.is_empty()
            && name.len() <= 64
            && name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
        {
            return Some(name.into());
        }
    }
    None
}

fn index_skills(cwd: &Path, home: Option<&Path>) -> Vec<String> {
    let mut roots = Vec::new();
    let mut current = Some(cwd);
    // Conservative local subset of the CLI's upward skill discovery.
    for _ in 0..=2 {
        let Some(dir) = current else {
            break;
        };
        if home.is_some_and(|h| dir == h) {
            break;
        }
        for sub in SKILL_DIRS {
            roots.push(dir.join(sub));
        }
        current = dir.parent();
    }
    if let Some(home) = home {
        roots.push(home.join(".tny/skills"));
        for sub in SKILL_DIRS.iter().filter(|s| s.starts_with('.')) {
            roots.push(home.join(sub));
        }
    }
    let mut skills = Vec::new();
    let mut seen = HashSet::new();
    for root in roots {
        if skills.len() >= INDEX_MAX_SKILLS {
            break;
        }
        if !has_no_symlink_components(&root)
            || !fs::symlink_metadata(&root).is_ok_and(|m| m.is_dir() && !m.file_type().is_symlink())
        {
            continue;
        }
        let Ok(entries) = fs::read_dir(root) else {
            continue;
        };
        for entry in entries.take(128).flatten() {
            if skills.len() >= INDEX_MAX_SKILLS {
                break;
            }
            if !entry
                .file_type()
                .is_ok_and(|ty| ty.is_dir() && !ty.is_symlink())
            {
                continue;
            }
            if let Some(name) = skill_name(&entry.path().join("SKILL.md")) {
                if seen.insert(name.clone()) {
                    skills.push(name);
                }
            }
        }
    }
    skills.sort_unstable();
    skills
}

fn local_index(cwd: &Path, home: Option<&Path>) -> LocalIndex {
    let (paths, paths_unavailable) = index_paths(cwd);
    LocalIndex {
        paths,
        skills: index_skills(cwd, home),
        ready: true,
        paths_unavailable,
    }
}

fn completion_context(draft: &str) -> Option<(CompletionKind, usize, &str)> {
    if draft.len() > 32 * 1024 {
        return None;
    }
    let command = draft.trim_start();
    if command.starts_with('/') && !command.chars().any(char::is_whitespace) {
        return Some((CompletionKind::Slash, draft.len() - command.len(), command));
    }
    for (idx, marker) in draft.char_indices().rev() {
        if marker != '@' && marker != '$' {
            continue;
        }
        let previous = draft[..idx].chars().next_back();
        if previous.is_some_and(|c| c.is_alphanumeric() || c == '_' || c == '`') {
            continue;
        }
        let query = &draft[idx + marker.len_utf8()..];
        if query.len() > 120 || query.contains('\n') || query.contains('\r') {
            continue;
        }
        if marker == '$' && query.chars().any(char::is_whitespace) {
            continue;
        }
        return Some((
            if marker == '@' {
                CompletionKind::File
            } else {
                CompletionKind::Skill
            },
            idx,
            query,
        ));
    }
    None
}

fn completions(draft: &str, index: &LocalIndex, remote: bool) -> (Vec<Completion>, String) {
    let Some((kind, start, query)) = completion_context(draft) else {
        return (Vec::new(), String::new());
    };
    if remote && kind != CompletionKind::Slash {
        return (
            Vec::new(),
            "Local paths and skills are unavailable over SSH; remote workspace is not indexed."
                .into(),
        );
    }
    if !index.ready && kind != CompletionKind::Slash {
        return (Vec::new(), "Indexing local workspace…".into());
    }
    if kind == CompletionKind::File && index.paths_unavailable {
        return (
            Vec::new(),
            "Git path index unavailable; private/ignored filenames are not scanned.".into(),
        );
    }
    let candidates: Vec<&str> = match kind {
        CompletionKind::File => index.paths.iter().map(String::as_str).collect(),
        CompletionKind::Skill => index.skills.iter().map(String::as_str).collect(),
        CompletionKind::Slash => GUI_COMMANDS.to_vec(),
    };
    let query_lower = query.to_ascii_lowercase();
    let mut matches: Vec<&str> = candidates
        .into_iter()
        .filter(|name| {
            let name = name.to_ascii_lowercase();
            if kind == CompletionKind::Slash {
                name.starts_with(&query_lower)
            } else {
                name.contains(&query_lower)
            }
        })
        .collect();
    matches.sort_unstable_by_key(|name| {
        (
            !name.to_ascii_lowercase().starts_with(&query_lower),
            name.to_ascii_lowercase(),
        )
    });
    let results: Vec<Completion> = matches
        .into_iter()
        .take(COMPLETION_MAX)
        .map(|name| Completion {
            kind,
            label: name.into(),
            start,
            query: query.into(),
            replacement: match kind {
                CompletionKind::File => format!("`{name}` "),
                CompletionKind::Skill => format!("${name} "),
                CompletionKind::Slash => format!("{name} "),
            },
        })
        .collect();
    let note = if results.is_empty() {
        match kind {
            CompletionKind::File => "No path match in the bounded local index (depth 4).",
            CompletionKind::Skill => "No local skill name matches.",
            CompletionKind::Slash => "No desktop GUI command matches.",
        }
    } else if kind == CompletionKind::File && index.paths.len() >= INDEX_MAX_FILES {
        "Local path suggestions capped at 512 entries."
    } else {
        ""
    };
    (results, note.into())
}

fn insert_completion(draft: &str, candidate: &Completion) -> Option<String> {
    if candidate.start > draft.len() || !draft.is_char_boundary(candidate.start) {
        return None;
    }
    let (kind, start, query) = completion_context(draft)?;
    if kind != candidate.kind || start != candidate.start || query != candidate.query {
        return None;
    }
    Some(format!("{}{}", &draft[..start], candidate.replacement))
}

#[derive(Debug, PartialEq, Eq)]
enum ImageFailureState {
    Committed(PathBuf),
    NotCommitted,
    Unknown,
}

fn image_failure_state(failure: &ImageFailure, output: &Path) -> ImageFailureState {
    match (failure.committed, failure.path.as_deref()) {
        (Some(true), Some(path)) if path == output && path.is_absolute() => {
            ImageFailureState::Committed(path.to_path_buf())
        }
        (Some(false), _) => ImageFailureState::NotCommitted,
        _ => ImageFailureState::Unknown,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SavedSessionAccess {
    ReviewToolLocation,
    RunningReadOnly,
    ContinuationReadOnly,
}

fn saved_session_access(doc: &Value) -> SavedSessionAccess {
    if doc.get("continuation").is_some_and(|v| !v.is_null()) {
        SavedSessionAccess::ContinuationReadOnly
    } else if matches!(field(doc, "status"), "running" | "stale") {
        SavedSessionAccess::RunningReadOnly
    } else {
        // No session field establishes prior SSH location: every selected
        // saved session requires an explicit decision before another turn.
        SavedSessionAccess::ReviewToolLocation
    }
}

fn image_output_path(raw: &str) -> Option<PathBuf> {
    if raw.is_empty() || raw.trim() != raw || raw.len() > 4096 || raw.chars().any(char::is_control)
    {
        return None;
    }
    let output = PathBuf::from(raw);
    if !output.is_absolute() || output.file_name().is_none() {
        return None;
    }
    Some(output)
}

fn gui_command(prompt: &str) -> Option<&'static str> {
    let trimmed = prompt.trim();
    GUI_COMMANDS.iter().copied().find(|name| *name == trimmed)
}

fn update_suggestions(ui: &App, state: &Rc<RefCell<State>>, draft: &str) {
    let mut s = state.borrow_mut();
    let (results, note) = completions(draft, &s.local_index, s.ssh.is_some());
    let rows: Vec<SuggestionRow> = results
        .iter()
        .map(|item| SuggestionRow {
            label: item.label.clone().into(),
            detail: match item.kind {
                CompletionKind::File => "path",
                CompletionKind::Skill => "skill",
                CompletionKind::Slash => "GUI command",
            }
            .into(),
        })
        .collect();
    s.completion_source = draft.into();
    s.completions = results;
    ui.set_suggestions(ModelRc::from(Rc::new(VecModel::from(rows))));
    ui.set_completion_note(note.into());
}

fn build_local_index(ui: &App, state: &Rc<RefCell<State>>) {
    let cwd = {
        let mut s = state.borrow_mut();
        if !s.index_gate.request() {
            return;
        }
        s.cwd.clone()
    };
    let home = std::env::var_os("HOME").map(PathBuf::from);
    dispatch(
        ui.as_weak(),
        move || local_index(&cwd, home.as_deref()),
        move |ui, index| {
            let Some(state) = ui_state(ui) else {
                return;
            };
            let rerun = {
                let mut s = state.borrow_mut();
                s.local_index = index;
                s.index_gate.complete()
            };
            update_suggestions(ui, &state, &ui.get_draft());
            if rerun {
                build_local_index(ui, &state);
            }
        },
    );
}

fn field<'a>(value: &'a Value, key: &str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or("")
}

fn lowercase_hex_id(id: &str, len: usize) -> bool {
    id.len() == len
        && id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn rows_from_session(doc: &Value) -> Vec<ChatRow> {
    // session.json keeps the injected skill body in messages[].content and the
    // user-visible draft in skill_injections[].display, indexed by message.
    let displays: HashMap<usize, &str> = doc
        .get("skill_injections")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|entry| {
            Some((
                entry.get("message")?.as_u64()? as usize,
                entry.get("display")?.as_str()?,
            ))
        })
        .collect();
    let mut rows = Vec::new();
    if let Some(messages) = doc.get("messages").and_then(Value::as_array) {
        for (index, item) in messages.iter().enumerate() {
            let role = field(item, "role");
            if role != "user" && role != "assistant" {
                continue;
            }
            let text = displays
                .get(&index)
                .copied()
                .unwrap_or_else(|| field(item, "content"));
            if !text.is_empty() {
                rows.push(ChatRow {
                    role: if role == "user" { "You" } else { "tny" }.into(),
                    body: text.into(),
                    meta: "".into(),
                });
            }
        }
    }
    // The stored result is the only readable answer in some saved sessions.
    if !rows.iter().any(|r| r.role == "tny") {
        if let Some(text) = doc.pointer("/result/output").and_then(Value::as_str) {
            if !text.is_empty() {
                rows.push(ChatRow {
                    role: "tny".into(),
                    body: text.into(),
                    meta: "Saved result".into(),
                });
            }
        }
    }
    rows
}

fn saved_user_count(doc: &Value) -> usize {
    doc.get("messages")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|item| field(item, "role") == "user")
        .count()
}

fn reconcile_saved(doc: &Value, id: &str, prior_users: usize) -> Option<(Vec<ChatRow>, bool)> {
    if field(doc, "id") != id {
        return None;
    }
    Some((rows_from_session(doc), saved_user_count(doc) > prior_users))
}

fn sessions_from_json(value: &Value) -> Vec<SessionRow> {
    value
        .get("sessions")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|row| {
            let id = field(row, "id");
            if id.is_empty() {
                return None;
            }
            let title = field(row, "title");
            let status = field(row, "status");
            let turns = row.get("turns").and_then(Value::as_u64).unwrap_or(0);
            Some(SessionRow {
                id: id.into(),
                title: if title.is_empty() { "Untitled" } else { title }.into(),
                meta: format!("{turns} turns · {status}").into(),
            })
        })
        .collect()
}

/// Token counts at a glance: 842, 12.3k, 167.9M.
fn compact_count(n: u64) -> String {
    match n {
        0..=999 => n.to_string(),
        1_000..=999_949 => format!("{:.1}k", n as f64 / 1e3),
        999_950..=999_949_999 => format!("{:.1}M", n as f64 / 1e6),
        _ => format!("{:.1}B", n as f64 / 1e9),
    }
}

fn usage_label(value: &Value) -> String {
    match (
        value.get("input_tokens").and_then(Value::as_u64),
        value.get("output_tokens").and_then(Value::as_u64),
    ) {
        (Some(input), Some(output)) => format!(
            "{} in · {} out tokens",
            compact_count(input),
            compact_count(output)
        ),
        _ => "Tokens unavailable".into(),
    }
}

fn session_usage_label(doc: &Value) -> Option<String> {
    let usage = doc.get("usage")?;
    Some(format!(
        "Session {} in · {} out tokens",
        compact_count(usage.get("in")?.as_u64()?),
        compact_count(usage.get("out")?.as_u64()?),
    ))
}

const BOARD_BATCH_MAX: usize = 16;
const BOARD_BYTES_MAX: usize = 65_536;
const BOARD_MESSAGE_MAX: usize = 16_384;

fn board_messages(value: &Value, run: &str) -> Result<Vec<BoardMessage>, &'static str> {
    if !lowercase_hex_id(run, 32)
        || field(value, "kind") != "team_mailbox"
        || value.get("ok").and_then(Value::as_bool) != Some(true)
        || field(value, "run") != run
    {
        return Err("Invalid parent inbox response");
    }
    let messages = value
        .get("messages")
        .and_then(Value::as_array)
        .ok_or("Missing parent inbox messages")?;
    if messages.len() > BOARD_BATCH_MAX {
        return Err("Parent inbox batch is oversized");
    }
    let mut rows = Vec::with_capacity(messages.len());
    let mut total = 0usize;
    for message in messages {
        let id = message
            .get("id")
            .and_then(Value::as_str)
            .ok_or("Missing message id")?;
        if id.is_empty()
            || id.len() > 64
            || !id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
        {
            return Err("Invalid message id");
        }
        let publication = message
            .get("publication")
            .and_then(Value::as_str)
            .ok_or("Missing publication id")?;
        if publication.len() > 64
            || !publication
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
        {
            return Err("Invalid publication id");
        }
        let sender = message
            .get("sender")
            .and_then(Value::as_i64)
            .ok_or("Missing sender")?;
        if !(-1..=63).contains(&sender)
            || message.get("recipient").and_then(Value::as_i64) != Some(-1)
        {
            return Err("Not a parent inbox message");
        }
        let attempt = message
            .get("attempt")
            .and_then(Value::as_u64)
            .ok_or("Missing attempt")?;
        let sequence = message
            .get("sequence")
            .and_then(Value::as_u64)
            .ok_or("Missing sequence")?;
        if attempt == 0 || attempt > u32::MAX as u64 || sequence == 0 {
            return Err("Invalid message attempt or sequence");
        }
        let state = message
            .get("state")
            .and_then(Value::as_str)
            .ok_or("Missing message state")?;
        if !matches!(state, "queued" | "delivered" | "acknowledged" | "retired") {
            return Err("Invalid message state");
        }
        let text = message
            .get("text")
            .and_then(Value::as_str)
            .ok_or("Missing message text")?;
        if text.len() > BOARD_MESSAGE_MAX || text.contains('\0') {
            return Err("Oversized message text");
        }
        total += text.len();
        if total > BOARD_BYTES_MAX {
            return Err("Oversized parent inbox batch");
        }
        let who = if sender == -1 {
            "lead".to_string()
        } else {
            format!("task {sender}")
        };
        let publication = if publication.is_empty() {
            String::new()
        } else {
            format!(" · publication {publication}")
        };
        rows.push(BoardMessage {
            heading: format!("{who} → parent · attempt {attempt} · seq {sequence}{publication}")
                .into(),
            id: format!("id: {id}").into(),
            state: format!("State: {state}").into(),
            body: text.into(), // Slint Text, never markdown, shell, or tool input.
        });
    }
    Ok(rows)
}

fn allowance_label(value: &Value) -> String {
    match value
        .pointer("/codex_usage/weekly_remaining_percent")
        .and_then(Value::as_u64)
    {
        Some(left) => format!("Codex weekly {left}% left"),
        None => "Allowance unavailable".into(),
    }
}

fn safe_field<'a>(value: &'a Value, key: &str, fallback: &'a str) -> &'a str {
    value.get(key).and_then(Value::as_str).unwrap_or(fallback)
}

fn swarm_for_turn(resume: Option<&str>, requested: Option<u8>) -> Option<u8> {
    // The saved session owns its swarm snapshot; a GUI draft choice is only
    // applicable to the first turn of a new session.
    if resume.is_some() {
        None
    } else {
        requested
    }
}

fn swarm_rows(value: &Value) -> Vec<SwarmRow> {
    if field(value, "kind") == "job" && value.get("dag").and_then(Value::as_bool) == Some(true) {
        let id = field(value, "id");
        if !lowercase_hex_id(id, 32) {
            return Vec::new();
        }
        let state = safe_field(value, "state", "unknown");
        let verification = safe_field(value, "verification", "unverified");
        let mut rows = vec![
            SwarmRow {
                title: format!("Job · DAG · {state}").into(),
                detail: format!("id: {id}").into(),
            },
            SwarmRow {
                title: format!("Verification: {verification}").into(),
                detail: if matches!(state, "failed" | "interrupted") {
                    "Failed/interrupted run · inspect tasks; not accepted".into()
                } else {
                    "Job status, not acceptance".into()
                },
            },
        ];
        if let Some(items) = value.get("items").and_then(Value::as_array) {
            rows.extend(items.iter().take(4).enumerate().map(|(i, item)| {
                SwarmRow {
                    title: format!("#{} · {}", i, safe_field(item, "role", "worker")).into(),
                    detail: format!(
                        "{} · {}",
                        safe_field(item, "state", "unknown"),
                        safe_field(item, "verification", "unverified")
                    )
                    .into(),
                }
            }));
            if items.len() > 4 {
                rows.push(SwarmRow {
                    title: format!("+{} more members", items.len() - 4).into(),
                    detail: "Use tny jobs status for full details".into(),
                });
            }
        }
        return rows;
    }
    value
        .get("agents")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter(|a| a.get("running").and_then(Value::as_bool).unwrap_or(false))
        .take(8)
        .map(|a| SwarmRow {
            title: format!("Agent · {}", safe_field(a, "status", "unknown")).into(),
            detail: "Background session".into(),
        })
        .collect()
}

fn mailbox_label(value: &Value, id: &str) -> Option<String> {
    if !lowercase_hex_id(id, 32) || value.get("ok")?.as_bool()? != true || field(value, "run") != id
    {
        return None;
    }
    let capacity = value.get("capacity")?;
    let recipient = capacity.get("recipient")?.as_i64()?;
    let who = match recipient {
        -1 => "lead (-1)".into(),
        0..=63 => format!("task {recipient}"),
        _ => return None,
    };
    Some(format!(
        "Run {}… history {}/{} · recipient {who} pending {}/{}",
        &id[..8],
        capacity.get("history_used")?.as_u64()?,
        capacity.get("history_limit")?.as_u64()?,
        capacity.get("outstanding_used")?.as_u64()?,
        capacity.get("outstanding_limit")?.as_u64()?,
    ))
}

fn unbound_parent_names<'a>(names: impl IntoIterator<Item = &'a str>) -> bool {
    !names
        .into_iter()
        .any(|name| name == "TNY_NESTED" || name.starts_with("TNY_TEAM_"))
}

fn unbound_parent_environment() -> bool {
    let names: Vec<_> = std::env::vars_os()
        .map(|(name, _)| name.to_string_lossy().into_owned())
        .collect();
    unbound_parent_names(names.iter().map(String::as_str))
}

fn parent_inbox_eligible(value: &Value, run: &str, unbound: bool) -> bool {
    unbound
        && lowercase_hex_id(run, 32)
        && field(value, "kind") == "team_mailbox"
        && value.get("ok").and_then(Value::as_bool) == Some(true)
        && field(value, "run") == run
        && value.pointer("/capacity/recipient").and_then(Value::as_i64) == Some(-1)
}

fn clear_board(ui: &App, status: &str) {
    ui.set_board_has_run(false);
    ui.set_board_messages(ModelRc::from(Rc::new(VecModel::from(
        Vec::<BoardMessage>::new(),
    ))));
    ui.set_board_status(status.into());
}

fn app_error(ui: &App, operation: &str) {
    // Never render CLI stderr, raw JSON, shell commands or environment credentials.
    ui.set_error_text(
        format!("{operation} failed. Check tny configuration or run the CLI for details.").into(),
    );
    ui.set_error_visible(true);
}

fn validation_error(ui: &App, message: &'static str) {
    ui.set_error_text(message.into());
    ui.set_error_visible(true);
}

fn reset_pending_allowance(ui: &App) {
    if ui.get_allowance_label() == "Checking allowance…" {
        ui.set_allowance_label("".into());
    }
}

fn dispatch<F, T>(ui: Weak<App>, worker: impl FnOnce() -> T + Send + 'static, apply: F)
where
    F: FnOnce(&App, T) + Send + 'static,
    T: Send + 'static,
{
    thread::spawn(move || {
        let result = worker();
        let _ = slint::invoke_from_event_loop(move || {
            if let Some(ui) = ui.upgrade() {
                apply(&ui, result);
            }
        });
    });
}

fn refresh(ui: &App, state: &Rc<RefCell<State>>) {
    let (bridge, swarm_run, serial, generation) = {
        let mut s = state.borrow_mut();
        if !s.refresh_gate.request() {
            return;
        }
        s.refresh_serial += 1;
        (
            s.bridge.clone(),
            s.swarm_run.clone(),
            s.refresh_serial,
            s.generation,
        )
    };
    let run_snapshot = swarm_run.clone();
    let weak = ui.as_weak();
    dispatch(
        weak,
        move || {
            let agents = match swarm_run.as_deref() {
                Some(id) => bridge.run_status(id),
                None => bridge.command(&["agents", "--json"]),
            };
            // mailbox status is observational; inbox/read change delivery state.
            let mailbox = swarm_run.as_deref().map(|id| {
                (
                    id.to_owned(),
                    bridge.command(&["mailbox", "status", "--run", id, "--json"]),
                )
            });
            (
                bridge.command(&["sessions", "--json"]),
                bridge.command(&["usage", "--json"]),
                agents,
                mailbox,
            )
        },
        move |ui, (sessions, usage, agents, mailbox)| {
            let Some(state) = ui_state(ui) else {
                return;
            };
            let (apply, rerun) = {
                let mut s = state.borrow_mut();
                let same_context = s.refresh_serial == serial
                    && s.generation == generation
                    && s.swarm_run == run_snapshot;
                let pending = s.refresh_gate.complete();
                (same_context && !pending, pending || !same_context)
            };
            if apply {
                match sessions {
                    Ok(value) => {
                        let list = sessions_from_json(&value);
                        if let Some(id) = &state.borrow().session_id {
                            if let Some(row) = list.iter().find(|row| row.id.as_str() == id) {
                                ui.set_session_title(row.title.clone());
                            }
                        }
                        ui.set_sessions(ModelRc::from(Rc::new(VecModel::from(list.clone()))));
                        state.borrow_mut().sessions = list;
                    }
                    Err(_) => app_error(ui, "Session refresh"),
                }
                let selected_usage = {
                    let s = state.borrow();
                    s.session_id.as_ref().map(|_| s.session_usage.clone())
                };
                match selected_usage {
                    Some(Some(label)) => ui.set_usage_label(label.into()),
                    Some(None) => ui.set_usage_label("Session tokens unavailable".into()),
                    None => match usage {
                        Ok(value) => {
                            ui.set_usage_label(format!("Workspace {}", usage_label(&value)).into())
                        }
                        Err(_) => ui.set_usage_label("Workspace tokens unavailable".into()),
                    },
                }
                match agents {
                    Ok(value) => {
                        ui.set_swarms(ModelRc::from(Rc::new(VecModel::from(swarm_rows(&value)))))
                    }
                    Err(_) => {
                        ui.set_swarms(ModelRc::from(Rc::new(VecModel::from(
                            Vec::<SwarmRow>::new(),
                        ))));
                        app_error(ui, "Agent refresh");
                    }
                }
                let (label, eligible, unavailable) = match mailbox {
                    Some((id, Ok(value))) => {
                        let unbound = unbound_parent_environment();
                        let label = mailbox_label(&value, &id)
                            .unwrap_or_else(|| "Mailbox status unavailable".into());
                        let eligible = parent_inbox_eligible(&value, &id, unbound);
                        let note = if !unbound {
                            "Parent inbox unavailable inside an inherited team/nested environment."
                        } else if label != "Mailbox status unavailable" {
                            "This capacity belongs to a task recipient, not the parent inbox."
                        } else {
                            "Parent inbox unavailable until parent capacity is confirmed."
                        };
                        (label, eligible, note)
                    }
                    Some((_, Err(_))) => (
                        "Mailbox status unavailable".into(),
                        false,
                        if unbound_parent_environment() {
                            "Parent inbox unavailable; passive status could not confirm parent identity."
                        } else {
                            "Parent inbox unavailable inside an inherited team/nested environment."
                        },
                    ),
                    None => (
                        "Mailbox · select a run".into(),
                        false,
                        "Parent inbox not opened",
                    ),
                };
                ui.set_mailbox_label(label.into());
                state.borrow_mut().parent_inbox_authorized = eligible;
                ui.set_board_has_run(eligible);
                if !eligible && run_snapshot.is_some() {
                    clear_board(ui, unavailable);
                }
            }
            if rerun {
                refresh(ui, &state);
            }
        },
    );
}

fn refresh_allowance(ui: &App, state: &Rc<RefCell<State>>) {
    let (bridge, generation) = {
        let mut s = state.borrow_mut();
        if s.allowance_in_flight {
            return;
        }
        s.allowance_in_flight = true;
        (s.bridge.clone(), s.generation)
    };
    ui.set_allowance_label("Checking allowance…".into());
    dispatch(
        ui.as_weak(),
        move || bridge.command(&["status", "--json"]),
        move |ui, result| {
            let Some(state) = ui_state(ui) else {
                return;
            };
            let current = {
                let mut s = state.borrow_mut();
                s.allowance_in_flight = false;
                s.generation == generation
            };
            if !current {
                return;
            }
            match result {
                Ok(value) => ui.set_allowance_label(allowance_label(&value).into()),
                Err(_) => {
                    ui.set_allowance_label("Allowance unavailable".into());
                    app_error(ui, "Allowance refresh");
                }
            }
        },
    );
}

// The UI thread owns the only mutable State. Slint callbacks/closures only borrow it briefly.
thread_local! { static UI_STATE: RefCell<Option<Rc<RefCell<State>>>> = const { RefCell::new(None) }; }
fn ui_state(_: &App) -> Option<Rc<RefCell<State>>> {
    UI_STATE.with(|state| state.borrow().clone())
}

fn stream_event(ui: &App, state: &Rc<RefCell<State>>, generation: u64, event: Value) {
    let mut s = state.borrow_mut();
    if s.generation != generation {
        return;
    }
    let id = field(&event, "session_id");
    if lowercase_hex_id(id, 16) {
        s.session_id = Some(id.into());
        ui.set_current_session_id(id.into());
        ui.set_has_session(true);
    }
    match field(&event, "type") {
        "text_delta" => {
            let text = field(&event, "text");
            if !text.is_empty() {
                let pos = match s.current_response {
                    Some(pos) => pos,
                    None => {
                        let pos = s.rows.row_count();
                        s.rows.push(ChatRow {
                            role: "tny".into(),
                            body: "".into(),
                            meta: "Streaming…".into(),
                        });
                        s.current_response = Some(pos);
                        pos
                    }
                };
                if let Some(mut row) = s.rows.row_data(pos) {
                    let mut body = row.body.to_string();
                    body.push_str(text);
                    row.body = body.into();
                    s.rows.set_row_data(pos, row);
                    ui.invoke_jump_to_latest();
                }
            }
        }
        "tool_start" => {
            // Tool names/details and CLI stderr may carry workspace or credential data.
            ui.set_status_label("Using a tool…".into());
        }
        "usage" => ui.set_usage_label(format!("Turn {}", usage_label(&event)).into()),
        "permission_request" => {
            // CLI JSONL does not expose an owner approval channel to this GUI.
            ui.set_status_label("Approval requested; unattended CLI policy applies".into());
        }
        "error" => {
            s.turn_has_error = true;
            app_error(ui, "Agent turn");
        }
        "turn_end" => {
            if let Some(pos) = s.current_response {
                if let Some(mut row) = s.rows.row_data(pos) {
                    row.meta = "".into();
                    s.rows.set_row_data(pos, row);
                }
            }
            if event.get("stop_reason").and_then(Value::as_i64) != Some(0) {
                s.turn_has_error = true;
                app_error(ui, "Agent turn");
            }
        }
        _ => {}
    }
}

fn connection_label(ssh: Option<&str>) -> String {
    ssh.map_or_else(|| "This computer".into(), |host| format!("SSH · {host}"))
}

fn swarm_label(swarm: Option<u8>) -> String {
    swarm.map_or_else(|| "Solo".into(), |count| format!("Swarm of {count}"))
}

fn main() -> Result<(), slint::PlatformError> {
    let launch_cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let cwd = std::env::var_os("TNY_GUI_CWD")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            if launch_cwd.file_name().is_some_and(|name| name == "gui")
                && launch_cwd.join("../AGENTS.md").is_file()
            {
                launch_cwd.parent().unwrap_or(&launch_cwd).to_path_buf()
            } else {
                launch_cwd
            }
        });
    let binary = std::env::var_os("TNY_GUI_BINARY")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            let local = cwd.join("build/tny");
            if local.is_file() {
                local.canonicalize().unwrap_or(local)
            } else {
                PathBuf::from("tny")
            }
        });
    let rows = Rc::new(VecModel::from(Vec::<ChatRow>::new()));
    let state = Rc::new(RefCell::new(State {
        bridge: Bridge::new(binary.clone(), cwd.clone(), None, None),
        binary,
        cwd: cwd.clone(),
        ssh: None,
        session_id: None,
        session_usage: None,
        inbox_in_flight: false,
        parent_inbox_authorized: false,
        generation: 0,
        refresh_serial: 0,
        refresh_gate: RefreshGate::default(),
        index_gate: RefreshGate::default(),
        allowance_in_flight: false,
        busy: false,
        swarm: None,
        swarm_run: None,
        rows: rows.clone(),
        sessions: Vec::new(),
        current_response: None,
        turn_has_error: false,
        unconfirmed: false,
        requires_tool_confirmation: false,
        saved_read_only: false,
        image_gen_busy: false,
        generated_path: None,
        local_index: LocalIndex::default(),
        completion_source: String::new(),
        completions: Vec::new(),
        picker: Picker::default(),
    }));
    let ui = App::new()?;
    ui.set_chat(ModelRc::from(rows));
    ui.set_workspace_label(cwd.display().to_string().into());
    UI_STATE.with(|s| *s.borrow_mut() = Some(state.clone()));

    ui.on_refresh({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            if let Some(ui) = weak.upgrade() {
                refresh(&ui, &state);
                if state.borrow().ssh.is_none() {
                    build_local_index(&ui, &state);
                }
                {
                    let mut s = state.borrow_mut();
                    s.picker.catalogs.clear();
                    s.picker.catalog_failed.clear();
                }
                load_providers(&ui, &state);
            }
        }
    });
    ui.on_draft_edited({
        let state = state.clone();
        let weak = ui.as_weak();
        move |draft| {
            if let Some(ui) = weak.upgrade() {
                update_suggestions(&ui, &state, &draft);
            }
        }
    });
    ui.on_choose_suggestion({
        let state = state.clone();
        let weak = ui.as_weak();
        move |index| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let draft = ui.get_draft().to_string();
            let choice = {
                let s = state.borrow();
                if draft != s.completion_source {
                    return;
                }
                s.completions.get(index as usize).cloned()
            };
            if let Some(completion) = choice {
                if let Some(completed) = insert_completion(&draft, &completion) {
                    ui.set_draft(completed.clone().into());
                    update_suggestions(&ui, &state, &completed);
                }
            }
        }
    });
    ui.on_check_allowance({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            if let Some(ui) = weak.upgrade() {
                refresh_allowance(&ui, &state);
            }
        }
    });
    ui.on_clear_error({
        let weak = ui.as_weak();
        move || {
            if let Some(ui) = weak.upgrade() {
                ui.set_error_visible(false);
            }
        }
    });
    ui.on_new_session({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            if let Some(ui) = weak.upgrade() {
                if state.borrow().busy {
                    validation_error(
                        &ui,
                        "Finish the current turn before starting a new session.",
                    );
                    return;
                }
                let mut s = state.borrow_mut();
                s.generation += 1;
                reset_pending_allowance(&ui);
                s.session_id = None;
                ui.set_current_session_id("".into());
                ui.set_swarm_mode_label(swarm_label(s.swarm).into());
                s.session_usage = None;
                ui.set_usage_label("Workspace tokens · refresh to view".into());
                s.current_response = None;
                s.unconfirmed = false;
                s.requires_tool_confirmation = false;
                s.saved_read_only = false;
                ui.set_needs_tool_confirmation(false);
                ui.set_saved_read_only(false);
                ui.set_saved_read_only_note("".into());
                s.rows.set_vec(Vec::new());
                ui.set_session_title("New conversation".into());
                ui.set_has_session(false);
                ui.set_status_label("Ready · new session".into());
                ui.set_error_visible(false);
            }
        }
    });
    ui.on_select_session({
        let state = state.clone();
        let weak = ui.as_weak();
        move |index| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let (bridge, id, title, generation) = {
                let mut s = state.borrow_mut();
                if s.busy {
                    validation_error(&ui, "Finish the current turn before switching sessions.");
                    return;
                }
                let Some(row) = s.sessions.get(index as usize).cloned() else {
                    return;
                };
                s.generation += 1;
                reset_pending_allowance(&ui);
                s.busy = true;
                (
                    s.bridge.clone(),
                    row.id.to_string(),
                    row.title.to_string(),
                    s.generation,
                )
            };
            ui.set_status_label("Opening saved session…".into());
            ui.set_busy(true);
            let selected_id = id.clone();
            let weak = ui.as_weak();
            dispatch(
                weak,
                move || bridge.command(&["session", &id, "--json"]),
                move |ui, result| {
                    let Some(state) = ui_state(ui) else {
                        return;
                    };
                    let mut s = state.borrow_mut();
                    if s.generation != generation {
                        return;
                    }
                    s.busy = false;
                    ui.set_busy(false);
                    match result {
                        Ok(doc) if field(&doc, "id") == selected_id => {
                            let access = saved_session_access(&doc);
                            s.rows.set_vec(rows_from_session(&doc));
                            ui.invoke_jump_to_latest();
                            ui.set_current_session_id(selected_id.clone().into());
                            s.session_id = Some(selected_id);
                            s.swarm = None;
                            ui.set_swarm_enabled(false);
                            ui.set_swarm_mode_label("Saved setup".into());
                            // The picker follows the chat: show what it resumes with.
                            s.picker.choice = saved_choice(&doc);
                            render_picker(ui, &s.picker);
                            s.session_usage = session_usage_label(&doc);
                            ui.set_usage_label(s.session_usage.clone().unwrap_or_else(|| "Session tokens unavailable".into()).into());
                            s.unconfirmed = false;
                            s.requires_tool_confirmation = true;
                            s.saved_read_only = access != SavedSessionAccess::ReviewToolLocation;
                            ui.set_session_title(title.into());
                            ui.set_has_session(true);
                            ui.set_error_visible(false);
                            ui.set_saved_read_only(s.saved_read_only);
                            ui.set_needs_tool_confirmation(!s.saved_read_only);
                            match access {
                                SavedSessionAccess::RunningReadOnly => {
                                    ui.set_saved_read_only_note("Saved session is running or stale · read-only. Reselect after it finishes. Prior SSH target is unknown.".into());
                                    ui.set_status_label("Read-only · running or stale session".into());
                                }
                                SavedSessionAccess::ContinuationReadOnly => {
                                    ui.set_saved_read_only_note("Saved checkpoint requires explicit CLI recovery · read-only. Historical SSH location is unknown.".into());
                                    ui.set_status_label("Read-only · checkpoint recovery via CLI".into());
                                }
                                SavedSessionAccess::ReviewToolLocation => {
                                    ui.set_saved_read_only_note("".into());
                                    ui.set_status_label("Choose where tools run to continue this chat".into());
                                }
                            }
                        }
                        _ => app_error(ui, "Session inspection"),
                    }
                },
            );
        }
    });
    ui.on_confirm_tool_location({
        let state = state.clone();
        let weak = ui.as_weak();
        move |use_ssh| {
            let Some(ui) = weak.upgrade() else { return; };
            let mut s = state.borrow_mut();
            if s.busy || s.session_id.is_none() || !s.requires_tool_confirmation || s.saved_read_only {
                validation_error(&ui, "Saved session cannot run now; reselect after it is idle and confirm tool location.");
                return;
            }
            if use_ssh && s.ssh.is_none() {
                validation_error(&ui, "Configure SSH before selecting the saved session, then confirm that target.");
                return;
            }
            let host = if use_ssh { s.ssh.clone() } else { None };
            s.bridge = Bridge::new(s.binary.clone(), s.cwd.clone(), host.clone(), None);
            s.ssh = host.clone();
            s.requires_tool_confirmation = false;
            ui.set_needs_tool_confirmation(false);
            ui.set_ssh_configured(host.is_some());
            ui.set_connection_label(connection_label(host.as_deref()).into());
            ui.set_status_label(if host.is_some() {
                "Tools will run on the current SSH host when you continue".into()
            } else {
                "Tools will run on this computer when you continue".into()
            });
        }
    });
    ui.on_submit({
        let state = state.clone();
        let weak = ui.as_weak();
        move |text: SharedString| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let prompt = text.to_string();
            if prompt.trim().is_empty() {
                return;
            }
            if let Some(command) = gui_command(&prompt) {
                if state.borrow().busy {
                    validation_error(&ui, "Finish the current turn before a GUI command.");
                    return;
                }
                match command {
                    "/new" => ui.invoke_new_session(),
                    "/refresh" | "/usage" => refresh(&ui, &state),
                    "/help" => {
                        ui.set_status_label("GUI commands: /new /refresh /usage /help".into())
                    }
                    _ => unreachable!(),
                }
                ui.set_draft("".into());
                update_suggestions(&ui, &state, "");
                if command == "/help" {
                    ui.set_completion_note("GUI commands: /new /refresh /usage /help".into());
                }
                return;
            }
            if prompt.starts_with('/')
                && prompt
                    .split_whitespace()
                    .next()
                    .is_some_and(|word| !word[1..].contains('/'))
            {
                validation_error(
                    &ui,
                    "Unsupported GUI slash command. Use /help for supported commands.",
                );
                return;
            }
            // The visual field is authoritative. No detached queue can silently
            // disagree with the path the user sees at submission time.
            let image = ui.get_image_path().to_string();
            let image = if image.trim().is_empty() {
                None
            } else {
                Some(image)
            };
            let (bridge, resume, swarm, generation, before_rows, before_users) = {
                let mut s = state.borrow_mut();
                if s.busy {
                    return;
                }
                if s.saved_read_only {
                    validation_error(&ui, "Selected session is read-only. Reselect after completion or use explicit CLI checkpoint recovery.");
                    return;
                }
                if s.requires_tool_confirmation {
                    validation_error(&ui, "Choose where tools run (above the conversation) before continuing this saved chat.");
                    return;
                }
                if s.unconfirmed {
                    validation_error(
                        &ui,
                        "Previous turn is unconfirmed. Inspect the saved session before retrying.",
                    );
                    return;
                }
                let before_rows = s.rows.row_count();
                let before_users = (0..before_rows)
                    .filter(|&i| s.rows.row_data(i).is_some_and(|row| row.role == "You"))
                    .count();
                s.busy = true;
                s.turn_has_error = false;
                s.generation += 1;
                s.current_response = None;
                s.rows.push(ChatRow {
                    role: "You".into(),
                    body: prompt.clone().into(),
                    meta: "Sending…".into(),
                });
                (
                    s.bridge.clone().with_model(s.picker.choice.clone()),
                    s.session_id.clone(),
                    swarm_for_turn(s.session_id.as_deref(), s.swarm),
                    s.generation,
                    before_rows,
                    before_users,
                )
            };
            reset_pending_allowance(&ui);
            ui.set_busy(true);
            ui.set_draft("".into());
            update_suggestions(&ui, &state, "");
            ui.set_error_visible(false);
            ui.set_status_label("Agent is working…".into());
            ui.invoke_jump_to_latest();
            let weak = ui.as_weak();
            thread::spawn(move || {
                let mut observed_id = resume.clone();
                let weak_events = weak.clone();
                let on_event = |event: Value| {
                    if let Some(id) = event.get("session_id").and_then(Value::as_str) {
                        if lowercase_hex_id(id, 16) {
                            observed_id = Some(id.to_owned());
                        }
                    }
                    let weak = weak_events.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = weak.upgrade() {
                            if let Some(state) = ui_state(&ui) {
                                stream_event(&ui, &state, generation, event);
                            }
                        }
                    });
                };
                let result = if let Some(path) = image.as_deref() {
                    bridge.ask_stream_image(&prompt, resume.as_deref(), swarm, Some(path), on_event)
                } else {
                    bridge.ask_stream(&prompt, resume.as_deref(), swarm, on_event)
                };
                let saved = observed_id
                    .as_ref()
                    .map(|id| (id.clone(), bridge.command(&["session", id, "--json"])));
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(ui) = weak.upgrade() else {
                        return;
                    };
                    let Some(state) = ui_state(&ui) else {
                        return;
                    };
                    let mut s = state.borrow_mut();
                    if s.generation != generation {
                        return;
                    }
                    s.busy = false;
                    ui.set_busy(false);
                    let failed = result.is_err() || s.turn_has_error;
                    let mut accepted = false;
                    let mut confirmed = false;
                    if let Some((id, Ok(doc))) = saved.as_ref() {
                        if let Some((rows, persisted_user)) = reconcile_saved(doc, id, before_users)
                        {
                            s.rows.set_vec(rows);
                            s.current_response = None;
                            s.session_id = Some(id.clone());
                            ui.set_current_session_id(id.clone().into());
                            s.session_usage = session_usage_label(doc);
                            ui.set_usage_label(s.session_usage.clone().unwrap_or_else(|| "Session tokens unavailable".into()).into());
                            ui.set_has_session(true);
                            accepted = persisted_user;
                            confirmed = true;
                            ui.invoke_jump_to_latest();
                        }
                    }
                    if confirmed && !accepted && !failed {
                        s.unconfirmed = true;
                    }
                    if !confirmed && saved.is_none() && failed {
                        // No session identity was supplied: discard the optimistic
                        // user/assistant rows so a retry cannot duplicate unsaved chat.
                        while s.rows.row_count() > before_rows {
                            s.rows.remove(before_rows);
                        }
                    } else if !confirmed {
                        // A session may exist despite a lost read/stream. Do not
                        // manufacture a success or automatically repost the prompt.
                        s.unconfirmed = true;
                        if let Some(mut row) = s.rows.row_data(before_rows) {
                            row.meta = "Unconfirmed · inspect saved session".into();
                            s.rows.set_row_data(before_rows, row);
                        }
                    }
                    if failed && !accepted && !s.unconfirmed && ui.get_draft().is_empty() {
                        ui.set_draft(prompt.into());
                    }
                    if (result.is_ok() || accepted)
                        && image.as_deref() == Some(ui.get_image_path().as_str())
                    {
                        ui.set_image_path("".into());
                    }
                    if failed {
                        app_error(&ui, "Agent turn");
                        ui.set_status_label(if s.unconfirmed {
                            "Turn unconfirmed · inspect session before retry".into()
                        } else {
                            if confirmed {
                                "Turn failed · saved transcript reconciled".into()
                            } else {
                                "Turn failed · prompt restored; check sessions before retry".into()
                            }
                        });
                    } else if s.unconfirmed {
                        validation_error(
                            &ui,
                            "Turn ended, but saved transcript could not be confirmed.",
                        );
                        ui.set_status_label("Inspect session before sending again".into());
                    } else {
                        ui.set_status_label("Ready".into());
                    }
                    drop(s);
                    update_suggestions(&ui, &state, &ui.get_draft());
                    refresh(&ui, &state);
                });
            });
        }
    });
    ui.on_optimise({
        let state = state.clone();
        let weak = ui.as_weak();
        move |draft| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let draft = draft.to_string();
            if draft.trim().is_empty() {
                validation_error(&ui, "Write a draft before optimising it.");
                return;
            }
            let (bridge, generation) = {
                let s = state.borrow();
                (s.bridge.clone(), s.generation)
            };
            ui.set_status_label("Optimising draft…".into());
            let reviewed_draft = draft.clone();
            dispatch(
                ui.as_weak(),
                move || bridge.command(&["optimise", &draft, "--json"]),
                move |ui, result| {
                    let Some(state) = ui_state(ui) else {
                        return;
                    };
                    if state.borrow().generation != generation {
                        return;
                    }
                    if ui.get_draft().to_string() != reviewed_draft {
                        ui.set_status_label("Draft changed; optimisation was not applied".into());
                        return;
                    }
                    match result {
                        Ok(value) if !field(&value, "text").is_empty() => {
                            ui.set_draft(field(&value, "text").into());
                            update_suggestions(ui, &state, &ui.get_draft());
                            ui.set_status_label("Review the rewritten draft before sending".into());
                        }
                        _ => app_error(ui, "Draft optimisation"),
                    }
                },
            );
        }
    });
    ui.on_dictate({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let (bridge, generation) = {
                let s = state.borrow();
                (s.bridge.clone(), s.generation)
            };
            ui.set_status_label("Recording 10 seconds…".into());
            dispatch(
                ui.as_weak(),
                move || bridge.command(&["dictate", "--seconds", "10", "--json"]),
                move |ui, result| {
                    let Some(state) = ui_state(ui) else {
                        return;
                    };
                    if state.borrow().generation != generation {
                        return;
                    }
                    match result {
                        Ok(value) if !field(&value, "text").is_empty() => {
                            let draft = ui.get_draft().to_string();
                            ui.set_draft(
                                format!(
                                    "{}{}{}",
                                    draft,
                                    if draft.is_empty() { "" } else { " " },
                                    field(&value, "text")
                                )
                                .into(),
                            );
                            update_suggestions(ui, &state, &ui.get_draft());
                            ui.set_status_label("Review dictation before sending".into());
                        }
                        _ => app_error(ui, "Dictation"),
                    }
                },
            );
        }
    });
    ui.on_generate_image({
        let state = state.clone();
        let weak = ui.as_weak();
        move |description, destination| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let prompt = description.to_string();
            let output = image_output_path(&destination);
            if prompt.trim().is_empty() {
                validation_error(&ui, "Describe an image before generating it.");
                return;
            }
            let Some(output) = output else {
                validation_error(&ui, "Choose an absolute local image output path without leading or trailing spaces.");
                return;
            };
            let bridge = {
                let mut s = state.borrow_mut();
                if s.image_gen_busy {
                    return;
                }
                if s.ssh.is_some() {
                    validation_error(
                        &ui,
                        "Image generation requires a local workspace; disconnect SSH first.",
                    );
                    return;
                }
                s.image_gen_busy = true;
                s.bridge.clone()
            };
            ui.set_image_gen_busy(true);
            ui.set_status_label("Generating image…".into());
            let saved_path = output.clone();
            dispatch(
                ui.as_weak(),
                move || bridge.generate_image(&prompt, &output),
                move |ui, result| {
                    let Some(state) = ui_state(ui) else {
                        return;
                    };
                    let mut s = state.borrow_mut();
                    s.image_gen_busy = false;
                    ui.set_image_gen_busy(false);
                    match result {
                        Ok(value) if field(&value, "path") == saved_path.to_string_lossy() => {
                            s.generated_path = Some(saved_path.clone());
                            ui.set_generated_path(saved_path.to_string_lossy().to_string().into());
                            ui.set_image_gen_result("Saved successfully · not previewed".into());
                            ui.set_error_visible(false);
                            ui.set_status_label("Image saved · attach explicitly".into());
                        }
                        Err(failure) => {
                            match image_failure_state(&failure, &saved_path) {
                                ImageFailureState::Committed(path) => {
                                    s.generated_path = Some(path.clone());
                                    ui.set_generated_path(path.to_string_lossy().to_string().into());
                                    ui.set_image_gen_result("Artifact committed · finalization failed · not previewed".into());
                                    ui.set_status_label("Image committed; inspect artifact before attaching".into());
                                    validation_error(ui, "Image artifact committed, but finalization failed. Inspect before attaching.");
                                }
                                ImageFailureState::NotCommitted => {
                                    s.generated_path = None;
                                    ui.set_generated_path("".into());
                                    ui.set_image_gen_result("Generation failed · no artifact committed".into());
                                    ui.set_status_label("No new image was committed".into());
                                    validation_error(ui, "Image generation failed; no new artifact was committed.");
                                }
                                ImageFailureState::Unknown => {
                                    s.generated_path = None;
                                    ui.set_generated_path("".into());
                                    ui.set_image_gen_result("Generation failed · commit status unknown; inspect output path".into());
                                    ui.set_status_label("Image commit status unknown · check destination".into());
                                    validation_error(ui, "Image commit status is unknown. Inspect destination before retrying.");
                                }
                            }
                        }
                        Ok(_) => {
                            s.generated_path = None;
                            ui.set_generated_path("".into());
                            ui.set_image_gen_result("Unexpected image receipt · commit status unknown".into());
                            app_error(ui, "Image generation");
                        }
                    }
                },
            );
        }
    });
    ui.on_attach_generated({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            if let Some(path) = state.borrow().generated_path.as_ref() {
                ui.set_image_path(path.to_string_lossy().to_string().into());
                ui.set_status_label("Generated image selected for next prompt · no preview".into());
            }
        }
    });
    ui.on_apply_ssh({
        let state = state.clone();
        let weak = ui.as_weak();
        move |host| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let host = host.trim().to_string();
            if host.is_empty() || host.starts_with('-') || host.contains(char::is_whitespace) {
                validation_error(
                    &ui,
                    "Enter a valid SSH target (user@host[:port], no spaces).",
                );
                return;
            }
            let mut s = state.borrow_mut();
            if s.busy || s.session_id.is_some() {
                validation_error(
                    &ui,
                    "Start a new chat (and let any turn finish) before changing where tools run.",
                );
                return;
            }
            if s.swarm.is_some() || s.swarm_run.is_some() {
                validation_error(
                    &ui,
                    "Turn off swarm mode and clear run inspection before using SSH.",
                );
                return;
            }
            s.generation += 1;
            reset_pending_allowance(&ui);
            s.bridge = Bridge::new(s.binary.clone(), s.cwd.clone(), Some(host.clone()), None);
            s.ssh = Some(host.clone());
            ui.set_connection_label(connection_label(Some(&host)).into());
            ui.set_ssh_configured(true);
            ui.set_status_label("Tools will run on this SSH host from the next message".into());
            ui.set_error_visible(false);
            drop(s);
            update_suggestions(&ui, &state, &ui.get_draft());
        }
    });
    ui.on_disconnect_ssh({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let mut s = state.borrow_mut();
            if s.ssh.is_none() {
                ui.set_status_label("Tools already run on this computer".into());
                return;
            }
            if s.busy || s.session_id.is_some() {
                validation_error(
                    &ui,
                    "Start a new chat (and let any turn finish) before changing where tools run.",
                );
                return;
            }
            s.generation += 1;
            reset_pending_allowance(&ui);
            s.bridge = Bridge::new(s.binary.clone(), s.cwd.clone(), None, None);
            s.ssh = None;
            ui.set_connection_label(connection_label(None).into());
            ui.set_ssh_configured(false);
            ui.set_status_label("Tools will run on this computer from the next message".into());
            drop(s);
            update_suggestions(&ui, &state, &ui.get_draft());
        }
    });
    ui.on_run_edited({
        let state = state.clone();
        let weak = ui.as_weak();
        move |text| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let mut s = state.borrow_mut();
            if s.swarm_run.as_deref() == Some(text.trim()) {
                return;
            }
            s.swarm_run = None;
            s.parent_inbox_authorized = false;
            drop(s);
            clear_board(&ui, "Run changed · inspect run before opening parent inbox");
            ui.set_mailbox_label("Mailbox · select a run".into());
            ui.set_swarms(ModelRc::from(Rc::new(VecModel::from(
                Vec::<SwarmRow>::new(),
            ))));
        }
    });
    ui.on_inspect_swarm({
        let state = state.clone();
        let weak = ui.as_weak();
        move |run| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let run = run.trim().to_string();
            if !run.is_empty() && state.borrow().ssh.is_some() {
                validation_error(
                    &ui,
                    "Run inspection and mailbox status require local tools.",
                );
                return;
            }
            if !run.is_empty() && !lowercase_hex_id(&run, 32) {
                validation_error(
                    &ui,
                    "Run ID must contain 32 lowercase hexadecimal characters.",
                );
                return;
            }
            let selected = if run.is_empty() { None } else { Some(run) };
            let changed = {
                let mut s = state.borrow_mut();
                let changed = s.swarm_run != selected;
                s.swarm_run = selected.clone();
                s.parent_inbox_authorized = false;
                changed
            };
            if changed {
                clear_board(&ui, "Parent inbox not opened for selected run");
            }
            ui.set_board_has_run(false);
            if selected.is_some() {
                ui.set_board_status(
                    "Checking passive parent capacity before inbox can open".into(),
                );
            }
            ui.set_mailbox_label(if selected.is_some() {
                "Refreshing mailbox capacity…".into()
            } else {
                "Mailbox · select a run".into()
            });
            ui.set_status_label("Refreshing swarm status…".into());
            refresh(&ui, &state);
        }
    });
    ui.on_open_parent_inbox({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            let Some(ui) = weak.upgrade() else { return; };
            let (bridge, run) = {
                let mut s = state.borrow_mut();
                if s.inbox_in_flight { return; }
                if !s.parent_inbox_authorized || !unbound_parent_environment() {
                    s.parent_inbox_authorized = false;
                    clear_board(&ui, "Parent inbox unavailable: passive status must confirm recipient lead (-1) from an unbound operator.");
                    return;
                }
                if s.ssh.is_some() {
                    validation_error(&ui, "Parent inbox requires a local run; SSH inbox is unavailable.");
                    return;
                }
                let Some(run) = s.swarm_run.clone() else {
                    validation_error(&ui, "Inspect a run before opening its parent inbox.");
                    return;
                };
                if ui.get_swarm_run().trim() != run || !lowercase_hex_id(&run, 32) {
                    validation_error(&ui, "Run selection changed; inspect it before opening parent inbox.");
                    return;
                }
                s.inbox_in_flight = true;
                (s.bridge.clone(), run)
            };
            ui.set_board_open_busy(true);
            ui.set_board_messages(ModelRc::from(Rc::new(VecModel::from(Vec::<BoardMessage>::new()))));
            ui.set_board_status("Opening parent inbox · may mark delivered; never acknowledges".into());
            let request_run = run.clone();
            dispatch(
                ui.as_weak(),
                move || bridge.command(&["mailbox", "inbox", "--run", &request_run, "--json"]),
                move |ui, result| {
                    let Some(state) = ui_state(ui) else { return; };
                    let mut s = state.borrow_mut();
                    s.inbox_in_flight = false;
                    ui.set_board_open_busy(false);
                    if s.swarm_run.as_deref() != Some(run.as_str()) { return; }
                    drop(s);
                    match result {
                        Ok(value) => match board_messages(&value, &run) {
                            Ok(rows) => {
                                let count = rows.len();
                                ui.set_board_messages(ModelRc::from(Rc::new(VecModel::from(rows))));
                                ui.set_board_status(format!("Opened {count} parent inbox messages · returned queued messages marked delivered; none acknowledged").into());
                            }
                            Err(_) => {
                                clear_board(ui, "Inbox response invalid; delivery outcome unknown. Inspect CLI before retry.");
                            }
                        },
                        Err(_) => {
                            clear_board(ui, "Inbox open failed; delivery outcome unknown. Inspect CLI before retry.");
                        }
                    }
                },
            );
        }
    });
    ui.on_change_swarm({
        let state = state.clone();
        let weak = ui.as_weak();
        move |count| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let mut s = state.borrow_mut();
            if s.busy || s.session_id.is_some() {
                validation_error(&ui, "Start a new idle session before changing swarm mode.");
                return;
            }
            if s.ssh.is_some() {
                validation_error(
                    &ui,
                    "Swarm mode requires local tools. Disconnect SSH first.",
                );
                return;
            }
            s.swarm = if count <= 1 {
                None
            } else {
                Some(count.min(16) as u8)
            };
            ui.set_swarm_enabled(s.swarm.is_some());
            ui.set_swarm_count(s.swarm.unwrap_or(2) as i32);
            ui.set_swarm_mode_label(swarm_label(s.swarm).into());
            ui.set_status_label(if s.swarm.is_some() {
                "Swarm enabled for the first message".into()
            } else {
                "Solo: one agent".into()
            });
        }
    });

    ui.on_open_model_picker({
        let state = state.clone();
        let weak = ui.as_weak();
        move || {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let loaded = state.borrow().picker.providers_loaded;
            if !loaded {
                state.borrow_mut().picker.models_wanted = true;
                load_providers(&ui, &state);
            }
            load_models(&ui, &state);
        }
    });
    ui.on_pick_provider({
        let state = state.clone();
        let weak = ui.as_weak();
        move |name| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            {
                let mut s = state.borrow_mut();
                let ready = s
                    .picker
                    .providers
                    .iter()
                    .any(|p| p.name == name.as_str() && p.ready);
                if !ready {
                    return;
                }
                let keep = s.picker.provider() == Some(name.as_str());
                if !keep {
                    s.picker.choice = Some(ModelChoice {
                        provider: name.to_string(),
                        model: None,
                    });
                }
                render_picker(&ui, &s.picker);
                ui.set_status_label(format!("Next message uses {}", s.picker.label()).into());
            }
            load_models(&ui, &state);
        }
    });
    ui.on_pick_model({
        let state = state.clone();
        let weak = ui.as_weak();
        move |id| {
            let Some(ui) = weak.upgrade() else {
                return;
            };
            let mut s = state.borrow_mut();
            let Some(provider) = s.picker.provider().map(str::to_owned) else {
                validation_error(&ui, "Choose a provider first.");
                return;
            };
            let id = id.trim();
            let choice = ModelChoice {
                provider,
                model: (!id.is_empty()).then(|| id.to_owned()),
            };
            if !choice.is_valid() {
                validation_error(
                    &ui,
                    "That model ID can't be used (empty, control characters or a leading dash).",
                );
                return;
            }
            s.picker.choice = Some(choice);
            render_picker(&ui, &s.picker);
            ui.set_status_label(format!("Next message uses {}", s.picker.label()).into());
        }
    });

    refresh(&ui, &state);
    build_local_index(&ui, &state);
    load_providers(&ui, &state);
    // Track an explicitly selected durable run without consuming mailbox messages.
    // Chat events already stream, so ordinary startup makes no periodic provider request.
    let run_timer = slint::Timer::default();
    run_timer.start(slint::TimerMode::Repeated, Duration::from_secs(8), {
        let weak = ui.as_weak();
        let state = state.clone();
        move || {
            if state.borrow().swarm_run.is_some() {
                if let Some(ui) = weak.upgrade() {
                    refresh(&ui, &state);
                }
            }
        }
    });
    ui.run()
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn autocomplete_matches_and_inserts_paths_with_spaces() {
        assert_eq!(
            image_output_path("/tmp/generated image.png").unwrap(),
            PathBuf::from("/tmp/generated image.png")
        );
        assert!(image_output_path("relative image.png").is_none());
        assert!(image_output_path("/tmp/output.png ").is_none());
        assert!(image_output_path("/tmp/out\nput.png").is_none());
        let index = LocalIndex {
            paths: vec!["src/My file.rs".into(), "src/main.rs".into()],
            skills: vec!["review-work".into()],
            ready: true,
            paths_unavailable: false,
        };
        let draft = "Inspect @My fi";
        let (matches, note) = completions(draft, &index, false);
        assert!(note.is_empty());
        assert_eq!(matches.len(), 1);
        assert_eq!(
            insert_completion(draft, &matches[0]).as_deref(),
            Some("Inspect `src/My file.rs` ")
        );
        let (skills, _) = completions("Use $rev", &index, false);
        assert_eq!(
            insert_completion("Use $rev", &skills[0]).as_deref(),
            Some("Use $review-work ")
        );
        let (commands, _) = completions("/re", &index, false);
        assert_eq!(
            insert_completion("/re", &commands[0]).as_deref(),
            Some("/refresh ")
        );
        assert_eq!(gui_command(" /refresh "), Some("/refresh"));
        // A click on a stale result must not change a newer draft.
        assert!(insert_completion("Other @entry", &matches[0]).is_none());
        assert!(insert_completion("Inspect @wrong", &matches[0]).is_none());
        assert!(completions("email name@host", &index, false).0.is_empty());
        assert!(completions("See @missing", &index, false)
            .1
            .contains("bounded local index"));
    }

    #[test]
    fn ssh_completion_never_labels_local_paths_or_skills_as_remote() {
        let index = LocalIndex {
            paths: vec!["src/main.rs".into()],
            skills: vec!["review".into()],
            ready: true,
            paths_unavailable: false,
        };
        for draft in ["See @main", "Use $rev"] {
            let (matches, note) = completions(draft, &index, true);
            assert!(matches.is_empty());
            assert!(note.contains("unavailable over SSH"));
        }
        assert!(!completions("/n", &index, true).0.is_empty());
        let (matches, note) = completions("See @main", &LocalIndex::default(), false);
        assert!(matches.is_empty());
        assert!(note.contains("Indexing"));
    }

    #[cfg(unix)]
    #[test]
    fn local_index_is_bounded_and_never_follows_symlinks() {
        use std::os::unix::fs::symlink;
        let fixture = std::env::temp_dir().join(format!(
            "tny-gui-index-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        let workspace = fixture.join("workspace");
        let home = fixture.join("home");
        fs::create_dir_all(workspace.join(".agents/skills/reviewer")).unwrap();
        fs::create_dir_all(workspace.join("src")).unwrap();
        fs::create_dir_all(workspace.join("target")).unwrap();
        fs::create_dir_all(home.join("private/linked-skill")).unwrap();
        fs::write(workspace.join("src/My file.rs"), b"// workspace file").unwrap();
        fs::write(workspace.join("target/app"), b"hidden").unwrap();
        fs::write(
            workspace.join(".agents/skills/reviewer/SKILL.md"),
            b"---\nname: review-work\n---\nbody",
        )
        .unwrap();
        fs::write(
            home.join("private/linked-skill/SKILL.md"),
            b"---\nname: should-not-appear\n---\n",
        )
        .unwrap();
        symlink(home.join("private"), workspace.join("src/linked-dir")).unwrap();
        symlink(
            home.join("private/linked-skill/SKILL.md"),
            workspace.join("src/linked-file.md"),
        )
        .unwrap();
        symlink(
            home.join("private/linked-skill"),
            workspace.join(".agents/skills/linked-skill"),
        )
        .unwrap();
        symlink(&workspace, home.join("linked-workspace")).unwrap();
        assert!(index_paths(&home.join("linked-workspace")).0.is_empty());
        let index = local_index(&workspace, Some(&home));
        assert!(index.ready);
        assert!(index.paths.contains(&"src/My file.rs".into()));
        assert!(index.paths.contains(&"src/".into()));
        assert!(!index
            .paths
            .iter()
            .any(|p| p.contains(".git") || p.contains("target") || p.contains("linked")));
        assert_eq!(index.skills, vec!["review-work"]);
        for i in 0..(INDEX_MAX_FILES + 32) {
            fs::write(workspace.join(format!("src/file-{i:04}.txt")), b"x").unwrap();
        }
        assert!(index_paths(&workspace).0.len() <= INDEX_MAX_FILES);
        // A broken repository is not classified as plain non-git: no fallback
        // can expose filenames that might have been ignored.
        fs::create_dir(workspace.join(".git")).unwrap();
        fs::write(workspace.join(".git/config"), b"broken").unwrap();
        assert_eq!(index_paths(&workspace), (Vec::new(), true));
        fs::remove_dir_all(&fixture).unwrap();
    }

    #[test]
    fn image_failure_receipts_are_distinct_and_never_assume_commit() {
        let output = PathBuf::from("/tmp/generated.png");
        let receipt = ImageFailure {
            committed: Some(true),
            path: Some(output.clone()),
            error: "finalization failed".into(),
        };
        assert_eq!(
            image_failure_state(&receipt, &output),
            ImageFailureState::Committed(output.clone())
        );
        let false_receipt = ImageFailure {
            committed: Some(false),
            path: None,
            error: "not written".into(),
        };
        assert_eq!(
            image_failure_state(&false_receipt, &output),
            ImageFailureState::NotCommitted
        );
        assert_eq!(
            image_failure_state(
                &ImageFailure {
                    path: Some(PathBuf::from("/other.png")),
                    ..receipt.clone()
                },
                &output
            ),
            ImageFailureState::Unknown
        );
        assert_eq!(
            image_failure_state(
                &ImageFailure {
                    committed: None,
                    ..receipt
                },
                &output
            ),
            ImageFailureState::Unknown
        );
    }

    #[test]
    fn saved_session_requires_tool_location_and_refuses_running_or_checkpoint() {
        assert_eq!(
            saved_session_access(&json!({"status":"done", "messages":[]})),
            SavedSessionAccess::ReviewToolLocation
        );
        assert_eq!(
            saved_session_access(&json!({"status":"error", "continuation":null})),
            SavedSessionAccess::ReviewToolLocation
        );
        assert_eq!(
            saved_session_access(&json!({"status":"running"})),
            SavedSessionAccess::RunningReadOnly
        );
        assert_eq!(
            saved_session_access(&json!({"status":"stale"})),
            SavedSessionAccess::RunningReadOnly
        );
        assert_eq!(
            saved_session_access(&json!({"status":"done","continuation":{"pending":true}})),
            SavedSessionAccess::ContinuationReadOnly
        );
    }

    #[test]
    fn refresh_gate_coalesces_repeated_requests() {
        let mut gate = RefreshGate::default();
        assert!(gate.request());
        for _ in 0..100 {
            assert!(!gate.request());
        }
        assert!(gate.complete());
        assert!(gate.request());
        assert!(!gate.complete());
        assert!(gate.request());
    }

    #[test]
    fn index_gate_finishes_once_and_rebuilds_once_after_repeated_requests() {
        let mut index_gate = RefreshGate::default();
        assert!(index_gate.request()); // initial indexing launched
        for _ in 0..10 {
            assert!(!index_gate.request());
        } // coalesced
        assert!(index_gate.complete()); // worker callback releases and schedules one
        assert!(index_gate.request()); // pending build actually launches
        assert!(!index_gate.complete()); // no further requests, no third build
        assert!(index_gate.request()); // later manual refresh can launch
    }

    #[cfg(unix)]
    #[test]
    fn git_paths_respect_excludes_without_scanning_private_files() {
        use std::os::unix::fs::symlink;
        let fixture = std::env::temp_dir().join(format!(
            "tny-gui-git-index-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        fs::create_dir(&fixture).unwrap();
        let status = Command::new("git")
            .args(["init", "-q"])
            .current_dir(&fixture)
            .status()
            .unwrap();
        assert!(status.success());
        fs::write(fixture.join(".gitignore"), b"secret.txt\ntarget/\n").unwrap();
        fs::write(fixture.join("public file.txt"), b"public").unwrap();
        fs::write(fixture.join("secret.txt"), b"private").unwrap();
        symlink("secret.txt", fixture.join("linked-secret.txt")).unwrap();
        fs::create_dir(fixture.join("target")).unwrap();
        fs::write(fixture.join("target/private.log"), b"private").unwrap();
        let (paths, unavailable) = index_paths(&fixture);
        assert!(!unavailable);
        assert!(paths.contains(&"public file.txt".into()));
        assert!(!paths
            .iter()
            .any(|p| p.contains("secret") || p.contains("target") || p.contains(".git/")));
        fs::remove_dir_all(&fixture).unwrap();
    }

    #[test]
    fn session_rows_and_transcript() {
        let sessions = sessions_from_json(
            &json!({"sessions":[{"id":"abc", "title":"First", "turns":2,"status":"done"},{"title":"invalid"}]}),
        );
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0].meta.as_str(), "2 turns · done");
        let transcript = rows_from_session(
            &json!({"messages":[{"role":"user","content":"hello"},{"role":"assistant","content":"world"},{"role":"tool","content":"secret"}]}),
        );
        assert_eq!(transcript.len(), 2);
        assert_eq!(transcript[1].body.as_str(), "world");
    }
    #[test]
    fn usage_unknown_is_not_zero() {
        assert_eq!(usage_label(&json!({})), "Tokens unavailable");
        assert_eq!(
            session_usage_label(&json!({"usage":{"in":8,"out":3}})).as_deref(),
            Some("Session 8 in · 3 out tokens")
        );
        assert_eq!(session_usage_label(&json!({"usage":null})), None);
        assert_eq!(session_usage_label(&json!({"usage":{"in":8}})), None);
        assert_eq!(
            allowance_label(&json!({"codex_usage":null})),
            "Allowance unavailable"
        );
        assert_eq!(
            usage_label(&json!({"input_tokens":12,"output_tokens":4})),
            "12 in · 4 out tokens"
        );
        assert_eq!(
            usage_label(&json!({"input_tokens":167_919_212u64,"output_tokens":637_762})),
            "167.9M in · 637.8k out tokens"
        );
        assert_eq!(compact_count(999), "999");
        assert_eq!(compact_count(1_000), "1.0k");
        assert_eq!(compact_count(999_949), "999.9k");
        assert_eq!(compact_count(999_950), "1.0M");
        assert_eq!(compact_count(2_500_000_000), "2.5B");
    }
    fn provider(name: &str, active: bool, ready: bool) -> ProviderInfo {
        ProviderInfo {
            name: name.into(),
            active,
            ready,
        }
    }

    #[test]
    fn picker_follows_cli_default_until_a_choice_is_made() {
        let mut picker = Picker::default();
        assert_eq!(picker.label(), "Default model");
        assert_eq!(picker.status(), "Loading providers…");
        picker.providers_loaded = true;
        assert!(picker.status().contains("No provider is configured"));
        picker.providers = vec![
            provider("openai", false, true),
            provider("codex", true, true),
            provider("acp", false, false),
        ];
        // No explicit choice: the CLI's active provider and its default model.
        assert_eq!(picker.provider(), Some("codex"));
        assert_eq!(picker.label(), "codex · Default");
        let rows = provider_rows(&picker);
        assert_eq!(rows[1].detail.as_str(), "Default · ready");
        assert_eq!(rows[2].detail.as_str(), "Needs setup");
        assert!(!rows[2].ready);
        picker.catalog_in_flight.insert("codex".into());
        assert_eq!(picker.status(), "Loading models…");
        picker.catalog_in_flight.clear();
        picker.catalogs.insert(
            "codex".into(),
            vec![ModelInfo {
                id: "gpt-6-astra".into(),
                name: "GPT-6-Astra".into(),
                description: "Frontier".into(),
            }],
        );
        assert_eq!(picker.status(), "");
        picker.choice = Some(ModelChoice {
            provider: "codex".into(),
            model: Some("gpt-6-astra".into()),
        });
        // Catalog display names label the pill; ids stay the CLI argument.
        assert_eq!(picker.label(), "codex · GPT-6-Astra");
        let models = model_rows(&picker);
        assert_eq!(models.len(), 1);
        assert_eq!(models[0].detail.as_str(), "Frontier");
        picker.choice = Some(ModelChoice {
            provider: "openai".into(),
            model: None,
        });
        picker.catalog_failed.insert("openai".into());
        assert_eq!(picker.label(), "openai · Default");
        assert!(picker.status().contains("Couldn't load"));
        picker.catalogs.insert("grok".into(), Vec::new());
        picker.choice = Some(ModelChoice {
            provider: "grok".into(),
            model: None,
        });
        assert!(picker.status().contains("doesn't publish"));
    }

    #[test]
    fn picker_keeps_typed_or_saved_models_outside_the_catalog_visible() {
        let mut picker = Picker::default();
        picker.catalogs.insert(
            "aiproxy".into(),
            vec![ModelInfo {
                id: "grok-4.6".into(),
                name: "grok-4.6".into(),
                description: "".into(),
            }],
        );
        picker.choice = Some(ModelChoice {
            provider: "aiproxy".into(),
            model: Some("custom/model".into()),
        });
        let rows = model_rows(&picker);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].id.as_str(), "custom/model");
        assert!(rows[0].detail.contains("Not in this provider's list"));
        // A plain id-only catalog row repeats nothing under the label.
        assert_eq!(rows[1].detail.as_str(), "");
        assert_eq!(picker.label(), "aiproxy · custom/model");
    }

    #[test]
    fn saved_choice_matches_cli_resume_fallbacks_and_rejects_flags() {
        assert_eq!(
            saved_choice(&json!({"backend":"codex","model":"gpt-6-luna"})),
            Some(ModelChoice {
                provider: "codex".into(),
                model: Some("gpt-6-luna".into())
            })
        );
        assert_eq!(
            saved_choice(&json!({"id":"aabbccddeeff0011"})),
            Some(ModelChoice {
                provider: "openai".into(),
                model: None
            })
        );
        assert_eq!(saved_choice(&json!({"backend":"--ssh"})), None);
        assert_eq!(
            saved_choice(&json!({"backend":"codex","model":"--yolo"})),
            None
        );
        assert_eq!(saved_choice(&json!({"backend":7})), None);
        assert_eq!(connection_label(None), "This computer");
        assert_eq!(connection_label(Some("me@box")), "SSH · me@box");
        assert_eq!(swarm_label(None), "Solo");
        assert_eq!(swarm_label(Some(3)), "Swarm of 3");
    }

    #[test]
    fn swarm_only_reports_active_agents() {
        let rows = swarm_rows(
            &json!({"agents":[{"running":true,"session_id":"abc","status":"running"},{"running":false,"session_id":"def"}]}),
        );
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].detail.as_str(), "Background session");
    }
    #[test]
    fn selected_run_keeps_failed_and_interrupted_job_snapshots() {
        let run = "0123456789abcdef0123456789abcdef";
        for state in ["failed", "interrupted"] {
            // Bridge::run_status returns this direct `kind:job` document even
            // when `jobs status` exits 2, unlike agents --run.
            let doc = json!({"kind":"job","id":run,"dag":true,"state":state,
                "verification":"unverified","items":[{"index":0,"role":"worker",
                "state":state,"verification":"unverified"}]});
            let rows = swarm_rows(&doc);
            assert_eq!(rows.len(), 3);
            assert!(rows[0].title.contains(state));
            assert!(rows[0].title.contains("DAG"));
            assert_eq!(rows[0].detail.as_str(), format!("id: {run}"));
            assert_eq!(rows[1].title.as_str(), "Verification: unverified");
            assert!(rows[1].detail.contains("not accepted"));
            assert!(rows[2].detail.contains(state));
        }
        let not_a_job = json!({"kind":"agents","run":{"state":"failed"}});
        assert!(swarm_rows(&not_a_job).is_empty());
    }

    #[test]
    fn saved_session_never_inherits_new_session_swarm_flags() {
        assert_eq!(swarm_for_turn(None, Some(4)), Some(4));
        assert_eq!(swarm_for_turn(Some("aabbccddeeff0011"), Some(4)), None);
        assert_eq!(swarm_for_turn(Some("aabbccddeeff0011"), None), None);
        assert_eq!(swarm_for_turn(None, None), None);
    }

    #[test]
    fn skill_injection_uses_display_not_injected_body() {
        let session = json!({
            "id":"aabbccddeeff0011",
            "messages":[
                {"role":"user","content":"SYSTEM: private skill instructions and secrets"},
                {"role":"assistant","content":"done"}
            ],
            "skill_injections":[{"message":0,"skills":["example"],"display":"/example review this"}]
        });
        let rows = rows_from_session(&session);
        assert_eq!(rows[0].body.as_str(), "/example review this");
        assert!(!rows[0].body.contains("private skill"));
        assert_eq!(saved_user_count(&session), 1);
        let (saved, accepted) = reconcile_saved(&session, "aabbccddeeff0011", 0).unwrap();
        assert_eq!(saved.len(), 2);
        assert!(accepted);
        // A rejected turn has no additional persisted user message, so the
        // optimistic row must be replaced and only the draft may be retried.
        assert!(!reconcile_saved(&session, "aabbccddeeff0011", 1).unwrap().1);
        assert!(reconcile_saved(&session, "another-id", 0).is_none());
    }

    #[test]
    fn parent_inbox_parses_exact_message_json_without_interpreting_payload() {
        let run = "0123456789abcdef0123456789abcdef";
        // src/core/team_runtime.c message_json and team_mailbox response.
        let response = json!({
            "kind":"team_mailbox", "run":run, "ok":true, "messages":[{
                "id":"question-1", "publication":"broadcast-1", "sequence":7,
                "sender":2, "recipient":-1, "attempt":3, "state":"delivered",
                "text":"<script>alert(1)</script>\nNever execute this: $(rm -rf /)"
            }], "retired":0, "error":null
        });
        let rows = board_messages(&response, run).unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(
            rows[0].heading.as_str(),
            "task 2 → parent · attempt 3 · seq 7 · publication broadcast-1"
        );
        assert_eq!(rows[0].id.as_str(), "id: question-1");
        assert_eq!(rows[0].state.as_str(), "State: delivered");
        assert_eq!(
            rows[0].body.as_str(),
            "<script>alert(1)</script>\nNever execute this: $(rm -rf /)"
        );
        assert!(board_messages(&response, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa").is_err());
        let mut wrong = response.clone();
        wrong["messages"][0]["recipient"] = json!(0);
        assert!(board_messages(&wrong, run).is_err());
        wrong["messages"][0]["recipient"] = json!(-1);
        wrong["ok"] = json!(false);
        assert!(board_messages(&wrong, run).is_err());
    }

    #[test]
    fn parent_inbox_bounds_messages_and_preserves_empty_inbox() {
        let run = "0123456789abcdef0123456789abcdef";
        let empty = json!({"kind":"team_mailbox","run":run,"ok":true,"messages":[],"retired":0,"error":null});
        assert!(board_messages(&empty, run).unwrap().is_empty());
        let item = json!({"id":"msg-1","publication":"","sequence":1,"sender":-1,
            "recipient":-1,"attempt":1,"state":"queued","text":"text"});
        let over = json!({"kind":"team_mailbox","run":run,"ok":true,
            "messages":vec![item.clone(); BOARD_BATCH_MAX + 1]});
        assert!(board_messages(&over, run).is_err());
        let mut oversized_item = item;
        oversized_item["text"] = json!("x".repeat(BOARD_MESSAGE_MAX + 1));
        let oversized = json!({"kind":"team_mailbox","run":run,"ok":true,
            "messages":[oversized_item]});
        assert!(board_messages(&oversized, run).is_err());
    }

    #[test]
    fn mailbox_status_is_capacity_only_and_fail_closed() {
        assert!(lowercase_hex_id("0123456789abcdef", 16));
        assert!(!lowercase_hex_id("ABCDEF0123456789", 16));
        let snapshot = json!({"kind":"team_mailbox","run":"0123456789abcdef0123456789abcdef",
          "ok":true,"messages":[],"capacity":{"history_used":3,"history_limit":256,
          "recipient":-1,"outstanding_used":2,"outstanding_limit":64}});
        assert_eq!(
            mailbox_label(&snapshot, "0123456789abcdef0123456789abcdef").unwrap(),
            "Run 01234567… history 3/256 · recipient lead (-1) pending 2/64"
        );
        let mut worker = snapshot.clone();
        worker["capacity"]["recipient"] = json!(2);
        assert!(mailbox_label(&worker, "0123456789abcdef0123456789abcdef")
            .unwrap()
            .contains("recipient task 2 pending 2/64"));
        worker["capacity"]["recipient"] = json!(-2);
        assert!(mailbox_label(&worker, "0123456789abcdef0123456789abcdef").is_none());
        assert!(mailbox_label(&snapshot, "wrong-run").is_none());
        assert!(mailbox_label(&json!({"ok":false}), "wrong-run").is_none());
        let run = "0123456789abcdef0123456789abcdef";
        assert!(parent_inbox_eligible(&snapshot, run, true));
        assert!(!parent_inbox_eligible(&snapshot, run, false));
        assert!(!parent_inbox_eligible(
            &snapshot,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            true
        ));
        let mut worker = snapshot.clone();
        worker["capacity"]["recipient"] = json!(0);
        assert!(!parent_inbox_eligible(&worker, run, true));
        worker["capacity"]["recipient"] = json!(-1);
        worker["ok"] = json!(false);
        assert!(!parent_inbox_eligible(&worker, run, true));
        assert!(unbound_parent_names(["HOME", "TNY_GUI_CWD"]));
        assert!(!unbound_parent_names(["TNY_NESTED"]));
        assert!(!unbound_parent_names(["TNY_TEAM_RUN"]));
    }
}
