#[cfg(unix)]
mod tests {
    use super::*;
    use std::fs;
    use std::os::unix::fs::PermissionsExt;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT: AtomicU64 = AtomicU64::new(0);

    struct Fixture {
        dir: PathBuf,
        binary: PathBuf,
    }

    impl Fixture {
        fn new(body: &str) -> Self {
            let dir = std::env::temp_dir().join(format!(
                "tny-gui-bridge-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&dir).unwrap();
            let binary = dir.join("tny fake");
            fs::write(&binary, format!("#!/bin/sh\nset -eu\n{body}\n")).unwrap();
            let mut perms = fs::metadata(&binary).unwrap().permissions();
            perms.set_mode(0o700);
            fs::set_permissions(&binary, perms).unwrap();
            Self { dir, binary }
        }

        fn bridge(&self) -> Bridge {
            Bridge::new(self.binary.clone(), self.dir.clone(), None, None)
        }

        fn text(&self, name: &str) -> String {
            fs::read_to_string(self.dir.join(name)).unwrap()
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.dir).unwrap();
        }
    }

    #[test]
    fn stream_delivers_in_order_before_child_finishes() {
        let f = Fixture::new(
            "cat >/dev/null\nprintf '{\"type\":\"status\",\"text\":\"waiting\"}\\n'\n\
             while [ ! -f marker ]; do sleep 0.01; done\n\
             printf '{\"type\":\"text_delta\",\"text\":\"hello\"}\\n'\n\
             printf '{\"type\":\"turn_end\",\"stop_reason\":0}\\n'",
        );
        let mut received = Vec::new();
        f.bridge()
            .ask_stream("plain text", None, None, |event| {
                if received.is_empty() {
                    fs::write(f.dir.join("marker"), "continue").unwrap();
                }
                received.push(event["type"].as_str().unwrap().to_owned());
            })
            .unwrap();
        assert_eq!(received, ["status", "text_delta", "turn_end"]);
    }

    #[test]
    fn non_success_stop_and_missing_terminal_fail() {
        for (body, expected) in [
            (
                "cat >/dev/null\nprintf '{\"type\":\"turn_end\",\"stop_reason\":4}\\n'",
                "stopped without success",
            ),
            (
                "cat >/dev/null\nprintf '{\"type\":\"status\"}\\n'",
                "without a turn_end",
            ),
        ] {
            let f = Fixture::new(body);
            let err = f
                .bridge()
                .ask_stream("text", None, None, |_| {})
                .unwrap_err();
            assert!(err.contains(expected), "{err}");
        }
    }

    #[test]
    fn child_failure_diagnostic_is_bounded_and_secret_free() {
        let secret = "PRIVATE_API_KEY=please-never-display-this";
        let f = Fixture::new(&format!(
            "printf '%s\\n' '{{\"kind\":\"ask_error\",\"code\":\"provider\",\"message\":\"{secret}\"}}' >&2\n\
             printf '%s\\n' '{secret}' >&2\nexit 2"
        ));
        let err = f
            .bridge()
            .ask_stream("text", None, None, |_| {})
            .unwrap_err();
        assert!(err.contains("exit 2, provider"), "{err}");
        assert!(!err.contains(secret));
        let err = f.bridge().command(&["status", "--json"]).unwrap_err();
        assert!(!err.contains(secret));
        assert!(err.contains("exit 2"));
    }

    #[test]
    fn arguments_and_stdin_are_literal_and_globals_lead() {
        let f = Fixture::new(
            "printf '%s\\n' \"$@\" > args\ncat > prompt\n\
             printf '{\"type\":\"turn_end\",\"stop_reason\":0}\\n'",
        );
        let bridge = Bridge::new(
            f.binary.clone(),
            f.dir.clone(),
            Some("user@example.com:2222".into()),
            Some("~/a folder; $HOME".into()),
        );
        let prompt = "--model injected; $(echo no-shell)\nnext line";
        bridge.ask_stream(prompt, None, None, |_| {}).unwrap();
        assert_eq!(f.text("prompt"), prompt);
        assert_eq!(
            f.text("args").lines().collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "--ssh",
                "user@example.com:2222",
                "--ssh-cwd",
                "~/a folder; $HOME",
                "ask",
                "--events=jsonl",
                "--progress=none",
                "--stdin"
            ]
        );
    }

    #[test]
    fn command_json_and_draft_starting_with_flag_are_literal() {
        let f = Fixture::new(
            "printf '%s\\n' \"$@\" > args\ncat > draft\nprintf '{\"kind\":\"optimise\",\"text\":\"ok\"}\\n'",
        );
        let draft = "--provider $(echo ignored); literal\nsecond line";
        let value = f.bridge().command(&["optimise", draft, "--json"]).unwrap();
        assert_eq!(value["text"], "ok");
        assert_eq!(f.text("draft"), draft);
        assert_eq!(
            f.text("args").lines().collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "optimise",
                "--stdin",
                "--json"
            ]
        );
    }

    #[test]
    fn image_generate_uses_cli_with_absolute_literal_output_and_prompt_stdin() {
        let f = Fixture::new(
            "printf '%s\\n' \"$@\" > args\ncat > prompt\n\
             printf '{\"kind\":\"image\",\"ok\":true,\"operation\":\"generate\",\"path\":\"%s\"}\\n' \"$6\"",
        );
        let output = f.dir.join("out ; literal.png");
        let prompt = "--size $(ignored)\nA blue robot";
        let result = f.bridge().generate_image(prompt, &output).unwrap();
        assert_eq!(result["kind"], "image");
        assert_eq!(result["path"], output.to_str().unwrap());
        assert_eq!(f.text("prompt"), prompt);
        assert_eq!(
            f.text("args").lines().collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "image",
                "generate",
                "--output-file",
                output.to_str().unwrap(),
                "--json"
            ]
        );
    }

    #[test]
    fn image_generate_validates_before_spawning_and_never_deletes_output() {
        let f = Fixture::new("echo spawned > marker");
        let output = f.dir.join("existing.png");
        fs::write(&output, "original").unwrap();
        let bridge = f.bridge();
        for prompt in ["", "   ", "bad\0prompt"] {
            assert!(bridge.generate_image(prompt, &output).is_err());
        }
        assert!(bridge
            .generate_image(&"x".repeat(16 * 1024 + 1), &output)
            .is_err());
        assert!(bridge
            .generate_image("hello", Path::new("relative.png"))
            .is_err());
        assert!(bridge.generate_image("hello", Path::new("/")).is_err());
        let ssh = Bridge::new(
            f.binary.clone(),
            f.dir.clone(),
            Some("remote.example".into()),
            None,
        );
        assert!(ssh.generate_image("hello", &output).is_err());
        assert_eq!(f.text("existing.png"), "original");
        assert!(!f.dir.join("marker").exists());
    }

    #[test]
    fn image_generate_failure_and_bad_success_never_echo_secrets() {
        let secret = "SECRET_IMAGE_TOKEN=do-not-display";
        let f = Fixture::new(&format!(
            "cat >/dev/null\nprintf '%s\\n' '{secret}' >&2\nprintf '%s\\n' '{{\"kind\":\"image\",\"ok\":false,\"error\":\"{secret}\"}}'\nexit 2"
        ));
        let err = f
            .bridge()
            .generate_image("hello", &f.dir.join("out.png"))
            .unwrap_err();
        assert!(err.error.contains("exit 2"));
        assert!(!err.error.contains(secret));
        assert_eq!(err.committed, None);
        assert_eq!(err.path, None);
        let bad =
            Fixture::new("cat >/dev/null\nprintf '%s\\n' '{\"kind\":\"image\",\"ok\":false}'");
        let err = bad
            .bridge()
            .generate_image("hello", &bad.dir.join("out.png"))
            .unwrap_err();
        assert!(err.error.contains("invalid result"));
    }

    #[test]
    fn image_manifest_failure_retains_committed_artifact_without_echoing_raw_errors() {
        let f = Fixture::new(
            "cat >/dev/null\nprintf 'paid artifact' > \"$6\"\n\
             printf '{\"kind\":\"image\",\"ok\":false,\"operation\":\"generate\",\"code\":\"IMAGE_MANIFEST_FINALIZE_FAILED\",\"committed\":true,\"path\":\"%s\",\"error\":\"SECRET_TOKEN\"}\\n' \"$6\"\n\
             printf 'SECRET_TOKEN in stderr\\n' >&2\nexit 1",
        );
        let output = f.dir.join("paid output.png");
        let failure = f.bridge().generate_image("prompt", &output).unwrap_err();
        assert_eq!(failure.committed, Some(true));
        assert_eq!(failure.path.as_deref(), Some(output.as_path()));
        assert!(failure.error.contains("manifest"));
        assert!(!failure.error.contains("SECRET_TOKEN"));
        assert_eq!(f.text("paid output.png"), "paid artifact");
    }

    #[test]
    fn image_failure_confirmation_requires_known_code_and_matching_path() {
        let f = Fixture::new(
            "cat >/dev/null\nprintf '%s\\n' '{\"kind\":\"image\",\"ok\":false,\"operation\":\"generate\",\"code\":\"IMAGE_SIZE_MISMATCH\",\"committed\":false,\"path\":null}'\nexit 1",
        );
        let failure = f
            .bridge()
            .generate_image("prompt", &f.dir.join("out.png"))
            .unwrap_err();
        assert_eq!(failure.committed, Some(false));
        assert_eq!(failure.path, None);
        let bad = Fixture::new(
            "cat >/dev/null\nprintf '%s\\n' '{\"kind\":\"image\",\"ok\":false,\"operation\":\"generate\",\"code\":\"IMAGE_MANIFEST_FINALIZE_FAILED\",\"committed\":true,\"path\":\"/other/file.png\"}'\nexit 1",
        );
        let failure = bad
            .bridge()
            .generate_image("prompt", &bad.dir.join("out.png"))
            .unwrap_err();
        assert_eq!(failure.committed, None);
        assert_eq!(failure.path, None);
    }

    #[test]
    fn image_option_uses_a_local_file_and_keeps_paths_literal() {
        let f = Fixture::new(
            "printf '%s\\n' \"$@\" > args\ncat > prompt\n\
             printf '%s\\n' '{\"type\":\"turn_end\",\"stop_reason\":0}'",
        );
        let path = "--image ; literal.png";
        fs::write(f.dir.join(path), b"fake image: CLI validates content").unwrap();
        let ssh = Bridge::new(
            f.binary.clone(),
            f.dir.clone(),
            Some("example.org".into()),
            None,
        );
        let mut events = Vec::new();
        ssh.ask_stream_image("describe", None, None, Some(path), |e| events.push(e))
            .unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(f.text("prompt"), "describe");
        assert_eq!(
            f.text("args").lines().collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "--ssh",
                "example.org",
                "ask",
                "--events=jsonl",
                "--progress=none",
                "--stdin",
                "--image",
                path
            ]
        );
        // The original API remains a text-only wrapper.
        f.bridge()
            .ask_stream("text only", None, None, |_| {})
            .unwrap();
        assert!(!f.text("args").lines().any(|arg| arg == "--image"));
    }

    #[test]
    fn image_rejection_happens_before_spawning() {
        let f = Fixture::new("echo spawned > marker");
        let bridge = f.bridge();
        for image in ["", " ", "missing.png", "../missing.png", "\0bad"] {
            let err = bridge
                .ask_stream_image("prompt", None, None, Some(image), |_| {})
                .unwrap_err();
            assert!(err.contains("local regular file"));
        }
        fs::create_dir(f.dir.join("directory.png")).unwrap();
        assert!(bridge
            .ask_stream_image("prompt", None, None, Some("directory.png"), |_| {})
            .is_err());
        assert!(!f.dir.join("marker").exists());
    }

    #[test]
    fn optimisation_failure_scrubs_stderr_and_requires_draft_delivery() {
        let f =
            Fixture::new("cat >/dev/null\nprintf '%s\\n' 'SECRET_TOKEN=do-not-print' >&2\nexit 2");
        let err = f
            .bridge()
            .command(&["optimise", "draft", "--json"])
            .unwrap_err();
        assert!(err.contains("exit 2"));
        assert!(!err.contains("SECRET_TOKEN"));
    }

    #[test]
    fn mailbox_views_have_exact_read_scope_and_validate_run_id() {
        let f = Fixture::new("printf '%s\\n' \"$@\" > args\nprintf '{\"kind\":\"mailbox\"}\\n'");
        let run = "0123456789abcdef0123456789abcdef";
        let bridge = f.bridge();
        for action in ["status", "inbox"] {
            let result = bridge.command(&["mailbox", action, "--run", run, "--json"]);
            if action == "inbox" && inherited_team_identity() {
                assert!(result.is_err());
                continue;
            }
            assert_eq!(result.unwrap()["kind"], "mailbox");
            assert_eq!(
                f.text("args").lines().collect::<Vec<_>>(),
                [
                    "--cwd",
                    f.dir.to_str().unwrap(),
                    "mailbox",
                    action,
                    "--run",
                    run,
                    "--json"
                ]
            );
        }
        assert!(bridge.command(&["team", "status", run, "--json"]).is_err());
        assert!(bridge
            .command(&["mailbox", "ack", "--run", run, "--json"])
            .is_err());
        assert!(bridge
            .command(&["mailbox", "inbox", "--run", "../bad", "--json"])
            .is_err());
        let ssh = Bridge::new(f.binary.clone(), f.dir.clone(), Some("host".into()), None);
        assert!(ssh
            .command(&["mailbox", "status", "--run", run, "--json"])
            .is_ok());
        assert!(!f.text("args").lines().any(|arg| arg == "--ssh"));
    }

    #[test]
    fn inbox_refuses_inherited_team_identity_without_stripping_environment() {
        let binary = std::env::current_exe().unwrap();
        for (name, value) in [
            ("TNY_NESTED", "1"),
            ("TNY_TEAM_RUN", "0123456789abcdef0123456789abcdef"),
            ("TNY_TEAM_TASK", "0"),
            ("", ""),
        ] {
            let mut child = Command::new(&binary);
            child.args([
                "--exact",
                "bridge::tests::inbox_identity_child",
                "--nocapture",
            ]);
            for (key, _) in std::env::vars_os() {
                if key == "TNY_NESTED" || key.to_string_lossy().starts_with("TNY_TEAM_") {
                    child.env_remove(key);
                }
            }
            child.env(
                "TNY_GUI_INBOX_CHILD",
                if name.is_empty() { "unbound" } else { "bound" },
            );
            if !name.is_empty() {
                child.env(name, value);
            }
            let output = child.output().unwrap();
            assert!(output.status.success(), "identity child failed");
        }
    }

    #[test]
    fn inbox_identity_child() {
        let Some(scope) = std::env::var_os("TNY_GUI_INBOX_CHILD") else {
            return;
        };
        let f = Fixture::new("echo spawned > marker\nprintf '%s\\n' '{\"kind\":\"mailbox\",\"capacity\":{\"recipient\":0}}'");
        let bridge = f.bridge();
        let run = "0123456789abcdef0123456789abcdef";
        let inbox = bridge.command(&["mailbox", "inbox", "--run", run, "--json"]);
        if scope == "bound" {
            assert!(inbox.unwrap_err().contains("unbound local operator"));
            assert!(!f.dir.join("marker").exists());
        } else {
            assert!(inbox.is_ok());
            assert!(f.dir.join("marker").exists());
        }
        let status = bridge
            .command(&["mailbox", "status", "--run", run, "--json"])
            .unwrap();
        assert_eq!(status["capacity"]["recipient"], 0);
    }

    #[test]
    fn run_status_keeps_failed_and_interrupted_job_documents() {
        let run = "0123456789abcdef0123456789abcdef";
        for (state, rc) in [
            ("failed", 2),
            ("interrupted", 2),
            ("running", 0),
            ("succeeded", 0),
        ] {
            let f = Fixture::new(&format!(
                "printf '%s\\n' \"$@\" > args\n\
                 printf '%s\\n' '{{\"kind\":\"job\",\"id\":\"{run}\",\"dag\":true,\"state\":\"{state}\",\"items\":[{{\"index\":0,\"state\":\"{state}\"}}]}}'\n\
                 exit {rc}"
            ));
            let bridge = Bridge::new(
                f.binary.clone(),
                f.dir.clone(),
                Some("remote".into()),
                Some("~/remote".into()),
            );
            let doc = bridge.run_status(run).unwrap();
            assert_eq!(doc["state"], state);
            assert_eq!(doc["items"][0]["state"], state);
            assert_eq!(
                f.text("args").lines().collect::<Vec<_>>(),
                [
                    "--cwd",
                    f.dir.to_str().unwrap(),
                    "jobs",
                    "status",
                    run,
                    "--json"
                ]
            );
        }
    }

    #[test]
    fn run_status_rejects_invalid_id_rc_and_documents_without_leaking_output() {
        let run = "0123456789abcdef0123456789abcdef";
        let f = Fixture::new("echo spawned > marker");
        assert!(f.bridge().run_status("../../bad").is_err());
        assert!(f
            .bridge()
            .run_status("ABCDEF0123456789abcdef0123456789")
            .is_err());
        assert!(!f.dir.join("marker").exists());
        for (doc, rc) in [
            (
                format!(
                    "{{\"kind\":\"agents\",\"id\":\"{run}\",\"dag\":true,\"state\":\"failed\"}}"
                ),
                2,
            ),
            (
                format!("{{\"kind\":\"job\",\"id\":\"{run}\",\"dag\":false,\"state\":\"failed\"}}"),
                2,
            ),
            (
                format!(
                    "{{\"kind\":\"job\",\"id\":\"{}\",\"dag\":true,\"state\":\"failed\"}}",
                    "f".repeat(32)
                ),
                2,
            ),
            (
                format!(
                    "{{\"kind\":\"job\",\"id\":\"{run}\",\"dag\":true,\"state\":\"succeeded\"}}"
                ),
                2,
            ),
            (
                format!("{{\"kind\":\"job\",\"id\":\"{run}\",\"dag\":true,\"state\":\"failed\"}}"),
                0,
            ),
            (
                format!("{{\"kind\":\"job\",\"id\":\"{run}\",\"dag\":true,\"state\":\"failed\"}}"),
                1,
            ),
            ("NOT_JSON SECRET_TOKEN".to_string(), 2),
        ] {
            let bad = Fixture::new(&format!(
                "printf '%s\\n' '{}'\nprintf '%s\\n' SECRET_TOKEN >&2\nexit {rc}",
                doc
            ));
            let err = bad.bridge().run_status(run).unwrap_err();
            assert!(!err.contains("SECRET_TOKEN"));
        }
    }

    #[test]
    fn local_metadata_omits_ssh_while_workspace_services_keep_it() {
        let f = Fixture::new(
            "printf '%s\\n' \"$@\" > args\n\
             if [ \"$3\" = optimise ]; then cat >/dev/null; fi\n\
             printf '%s\\n' '{\"kind\":\"result\"}'",
        );
        let bridge = Bridge::new(
            f.binary.clone(),
            f.dir.clone(),
            Some("host".into()),
            Some("~/remote".into()),
        );
        let id = "0123456789abcdef";
        let run = "0123456789abcdef0123456789abcdef";
        for args in [
            vec!["sessions", "--json"],
            vec!["session", id, "--json"],
            vec!["usage", "--json"],
            vec!["status", "--json"],
            vec!["agents", "--json"],
            vec!["agents", "--run", run, "--json"],
            vec!["mailbox", "status", "--run", run, "--json"],
        ] {
            assert!(bridge.command(&args).is_ok());
            assert!(!f
                .text("args")
                .lines()
                .any(|arg| arg == "--ssh" || arg == "--ssh-cwd"));
        }
        assert!(bridge
            .command(&["dictate", "--seconds", "10", "--json"])
            .is_ok());
        assert_eq!(
            f.text("args").lines().take(6).collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "--ssh",
                "host",
                "--ssh-cwd",
                "~/remote"
            ]
        );
        // Optimise still requests remote project tools; it owns prompt stdin.
        assert!(bridge.command(&["optimise", "draft", "--json"]).is_ok());
        assert!(f.text("args").lines().any(|arg| arg == "--ssh"));
    }

    #[test]
    fn metadata_timeout_bounds_running_and_retained_pipe_children() {
        for script in [
            "sleep 2",
            "sleep 0.3 &\nprintf '%s\\n' '{\"kind\":\"status\"}'",
        ] {
            let f = Fixture::new(script);
            let mut cmd = f.bridge().process(false).unwrap();
            cmd.args(["status", "--json"]).stdin(Stdio::null());
            let start = Instant::now();
            let err = run_json(cmd, "status", None, Some(Duration::from_millis(50))).unwrap_err();
            assert!(err.contains("timed out"), "{err}");
            assert!(start.elapsed() < Duration::from_secs(1));
        }
    }

    #[test]
    fn rejects_untrusted_flags_ids_swarm_and_malformed_streams() {
        let f = Fixture::new("echo '{\"type\":\"turn_end\",\"stop_reason\":0}'");
        let bridge = f.bridge();
        assert!(bridge.command(&["login", "--json"]).is_err());
        assert!(bridge.command(&["session", "--help", "--json"]).is_err());
        assert!(bridge
            .command(&["agents", "--run", "../../abc", "--json"])
            .is_err());
        assert!(bridge.ask_stream("ok", Some("last"), None, |_| {}).is_err());
        assert!(bridge.ask_stream("ok", None, Some(17), |_| {}).is_err());
        let invalid_ssh = Bridge::new(
            f.binary.clone(),
            f.dir.clone(),
            Some("-oProxyCommand".into()),
            None,
        );
        assert!(invalid_ssh.command(&["status", "--json"]).is_ok());
        assert!(invalid_ssh.ask_stream("ok", None, None, |_| {}).is_err());
        let ssh_swarm = Bridge::new(f.binary.clone(), f.dir.clone(), Some("host".into()), None);
        assert!(ssh_swarm.ask_stream("ok", None, Some(2), |_| {}).is_err());
        let invalid = Fixture::new("printf '%s\\n' 'not-json'");
        assert!(invalid
            .bridge()
            .ask_stream("ok", None, None, |_| {})
            .is_err());
        let duplicate = Fixture::new("printf '%s\\n' '{\"type\":\"turn_end\",\"stop_reason\":0}' '{\"type\":\"turn_end\",\"stop_reason\":0}'");
        assert!(duplicate
            .bridge()
            .ask_stream("ok", None, None, |_| {})
            .is_err());
    }

    #[test]
    fn resumed_swarm_flags_and_fragmented_events() {
        let f = Fixture::new(
            "if [ \"$3\" = session ]; then\n\
               printf '%s\\n' '{\"id\":\"0123456789abcdef\",\"backend\":\"codex\",\"model\":\"gpt-5.6\"}'\n\
               exit 0\n\
             fi\n\
             printf '%s\\n' \"$@\" > args\ncat >/dev/null\n\
             printf '%s' '{\"type\":\"text_'\n\
             printf '%s\\n' 'delta\",\"text\":\"chunk\"}'\n\
             printf '%s\\n' '{\"type\":\"turn_end\",\"stop_reason\":0}'",
        );
        let id = "0123456789abcdef";
        let mut events = Vec::new();
        f.bridge()
            .ask_stream("prompt", Some(id), Some(3), |event| events.push(event))
            .unwrap();
        assert_eq!(events[0]["text"], "chunk");
        assert_eq!(
            f.text("args").lines().collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "--provider",
                "codex",
                "--model",
                "gpt-5.6",
                "ask",
                "--events=jsonl",
                "--progress=none",
                "--stdin",
                "--resume",
                id,
                "--swarm=3"
            ]
        );
    }

    #[test]
    fn resume_uses_legacy_provider_fallback_and_rejects_bad_saved_config() {
        let id = "0123456789abcdef";
        let f = Fixture::new(&format!(
            "if [ \"$3\" = session ]; then printf '%s\\n' '{{\"id\":\"{id}\"}}'; exit 0; fi\n\
             printf '%s\\n' \"$@\" > args\ncat >/dev/null\n\
             printf '%s\\n' '{{\"type\":\"turn_end\",\"stop_reason\":0}}'"
        ));
        f.bridge()
            .ask_stream("hello", Some(id), None, |_| {})
            .unwrap();
        assert_eq!(
            f.text("args").lines().take(5).collect::<Vec<_>>(),
            [
                "--cwd",
                f.dir.to_str().unwrap(),
                "--provider",
                "openai",
                "ask"
            ]
        );
        let invalid = Fixture::new(&format!(
            "if [ \"$3\" = session ]; then printf '%s\\n' '{{\"id\":\"{id}\",\"backend\":\"--bad\"}}'; exit 0; fi\n\
             echo spawned > marker"
        ));
        let err = invalid
            .bridge()
            .ask_stream("hello", Some(id), None, |_| {})
            .unwrap_err();
        assert!(err.contains("Invalid saved session provider"), "{err}");
        assert!(!invalid.dir.join("marker").exists());
    }

    #[test]
    fn nonzero_exit_overrides_success_event() {
        let f = Fixture::new("printf '%s\\n' '{\"type\":\"turn_end\",\"stop_reason\":0}'\nexit 2");
        let mut got_terminal = false;
        let err = f
            .bridge()
            .ask_stream("ok", None, None, |_| got_terminal = true)
            .unwrap_err();
        assert!(got_terminal);
        assert!(err.contains("exit 2"), "{err}");
    }

    #[test]
    fn bounded_json_and_stderr_never_surface_raw_output() {
        let f = Fixture::new("printf '{broken'\nprintf '%s\\n' 'TOP_SECRET_KEY' >&2");
        let err = f.bridge().command(&["status", "--json"]).unwrap_err();
        assert!(err.contains("invalid JSON"));
        assert!(!err.contains("TOP_SECRET_KEY"));
        let f = Fixture::new("head -c 16777217 /dev/zero");
        assert!(f.bridge().command(&["status", "--json"]).is_err());
        let f = Fixture::new("head -c 4194305 /dev/zero");
        assert!(f.bridge().ask_stream("ok", None, None, |_| {}).is_err());
        let f = Fixture::new("head -c 65536 /dev/zero >&2\nexit 1");
        let err = f.bridge().ask_stream("ok", None, None, |_| {}).unwrap_err();
        assert_eq!(err, "tny ask failed (exit 1)");
    }
}
