//! The working folder of the current chat and everything derived from it.
//!
//! Mirrors `proofs/Proofs/Workdir.lean`. tny sessions belong to their
//! workspace (`--resume` from another folder is refused), so a valid change
//! always starts a new chat; results computed for another folder (the `@` file
//! index, the Recent list) are dropped; and nothing changes while a turn runs.
//! The CLI bridge's `--cwd` and the visible label are rebuilt from `cwd` on
//! every accepted change, which is how `bridge = label = cwd` holds here.

use std::path::{Path, PathBuf};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Workdir<D> {
    pub cwd: D,
    /// The folder the applied `@` file index was built for.
    pub index: Option<D>,
    /// The folder the Recent sessions list was read for.
    pub list: Option<D>,
    /// The folder the open chat's session lives in.
    pub session: Option<D>,
    /// A turn is running.
    pub busy: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Event<D> {
    Change(D, bool),
    IndexArrived(D),
    ListArrived(D),
    Select,
    Submit,
    Finish,
    NewChat,
}

impl<D: Clone + PartialEq> Workdir<D> {
    pub fn new(cwd: D) -> Self {
        Self {
            cwd,
            index: None,
            list: None,
            session: None,
            busy: false,
        }
    }

    /// `Workdir.step`. Returns whether the event was applied.
    pub fn step(&mut self, event: Event<D>) -> bool {
        let next = match event {
            Event::Change(dir, valid) if !self.busy && valid => Self::new(dir),
            Event::Change(..) => return false,
            Event::IndexArrived(dir) if dir == self.cwd => Self {
                index: Some(dir),
                ..self.clone()
            },
            Event::ListArrived(dir) if dir == self.cwd => Self {
                list: Some(dir),
                ..self.clone()
            },
            Event::IndexArrived(_) | Event::ListArrived(_) => return false,
            Event::Select if !self.busy && self.list.as_ref() == Some(&self.cwd) => Self {
                session: Some(self.cwd.clone()),
                ..self.clone()
            },
            Event::Select => return false,
            Event::Submit if !self.busy => Self {
                busy: true,
                ..self.clone()
            },
            Event::Finish if self.busy => Self {
                busy: false,
                session: Some(self.cwd.clone()),
                ..self.clone()
            },
            Event::NewChat if !self.busy => Self {
                session: None,
                ..self.clone()
            },
            Event::Submit | Event::Finish | Event::NewChat => return false,
        };
        *self = next;
        true
    }
}

/// Resolve a folder typed by the user: `~` expands to `home`, the result must
/// be an existing absolute directory and is canonicalized (symlinks resolved),
/// so the label, `--cwd` and the session bucket all name the same folder.
pub fn resolve(raw: &str, home: Option<&Path>) -> Result<PathBuf, &'static str> {
    let raw = raw.trim();
    if raw.is_empty() {
        return Err("Enter a folder path.");
    }
    if raw.len() > 4096 || raw.chars().any(char::is_control) {
        return Err("That folder path contains control characters or is too long.");
    }
    let path = match raw.strip_prefix('~') {
        Some(rest) if rest.is_empty() || rest.starts_with('/') => match home {
            Some(home) => home.join(rest.trim_start_matches('/')),
            None => return Err("HOME is not set, so ~ cannot be expanded."),
        },
        _ => PathBuf::from(raw),
    };
    if !path.is_absolute() {
        return Err("Use an absolute folder path (or one starting with ~).");
    }
    let path = path
        .canonicalize()
        .map_err(|_| "That folder does not exist or cannot be opened.")?;
    if !path.is_dir() {
        return Err("That path is a file, not a folder.");
    }
    if path.to_str().is_none() {
        return Err("That folder path is not valid UTF-8.");
    }
    Ok(path)
}

/// The composer shows the folder's name; the tooltip shows the whole path.
pub fn short_label(path: &Path, home: Option<&Path>) -> String {
    if home.is_some_and(|h| h == path) {
        return "~".into();
    }
    path.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string())
}

/// `~/…` for folders under HOME, for readable tooltips and recent lists.
pub fn display(path: &Path, home: Option<&Path>) -> String {
    match home.and_then(|h| path.strip_prefix(h).ok()) {
        Some(rest) if rest.as_os_str().is_empty() => "~".into(),
        Some(rest) => format!("~/{}", rest.display()),
        None => path.display().to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(name: &str) -> bool {
        match name {
            "A" => false,
            "B" => true,
            _ => panic!("unknown folder {name}"),
        }
    }

    fn odir(name: &str) -> Option<bool> {
        (name != "-").then(|| dir(name))
    }

    fn state(cols: &[&str]) -> Workdir<bool> {
        Workdir {
            cwd: dir(cols[0]),
            index: odir(cols[1]),
            list: odir(cols[2]),
            session: odir(cols[3]),
            busy: cols[4] == "1",
        }
    }

    fn event(name: &str) -> Event<bool> {
        let parts: Vec<&str> = name.split(':').collect();
        match parts.as_slice() {
            ["change", d, v] => Event::Change(dir(d), *v == "1"),
            ["index", d] => Event::IndexArrived(dir(d)),
            ["list", d] => Event::ListArrived(dir(d)),
            ["select"] => Event::Select,
            ["submit"] => Event::Submit,
            ["finish"] => Event::Finish,
            ["new_chat"] => Event::NewChat,
            _ => panic!("unknown event {name}"),
        }
    }

    #[test]
    fn step_matches_every_row_of_the_proven_lean_table() {
        let table = include_str!("../proofs/golden/workdir.tsv");
        let mut count = 0;
        for line in table.lines().skip(1) {
            let cols: Vec<&str> = line.split('\t').collect();
            let mut s = state(&cols[0..5]);
            s.step(event(cols[5]));
            assert_eq!(s, state(&cols[6..11]), "{cols:?}");
            count += 1;
        }
        // 2 folders × 3 index × 3 list × 3 session... restricted to Workdir.Inv.
        assert_eq!(count, 384);
    }

    #[test]
    fn a_turn_freezes_the_folder_and_its_session_lands_there() {
        let mut w = Workdir::new("/a");
        assert!(w.step(Event::Submit));
        assert!(!w.step(Event::Change("/b", true)));
        assert!(w.step(Event::Finish));
        assert_eq!((w.cwd, w.session), ("/a", Some("/a")));
        assert!(w.step(Event::Change("/b", true)));
        assert_eq!(w, Workdir::new("/b"));
        assert!(!w.step(Event::IndexArrived("/a")), "stale index is dropped");
        assert!(!w.step(Event::Select), "no Recent list for /b yet");
    }

    #[test]
    fn resolve_requires_an_existing_absolute_directory() {
        let root = std::env::temp_dir().join(format!("tny-gui-workdir-{}", std::process::id()));
        let sub = root.join("a folder");
        std::fs::create_dir_all(&sub).unwrap();
        std::fs::write(root.join("file"), "").unwrap();
        let canon = sub.canonicalize().unwrap();
        assert_eq!(
            resolve(&format!("  {}  ", sub.display()), None),
            Ok(canon.clone())
        );
        assert_eq!(resolve("~/a folder", Some(&root)), Ok(canon.clone()));
        assert_eq!(resolve("~", Some(&root)), Ok(root.canonicalize().unwrap()));
        assert!(resolve("relative", None).is_err());
        assert!(resolve("~other", Some(&root)).is_err());
        assert!(resolve(&root.join("file").display().to_string(), None).is_err());
        assert!(resolve(&root.join("missing").display().to_string(), None).is_err());
        assert!(resolve("/tmp/a\nb", None).is_err());
        assert!(resolve("", None).is_err());
        assert_eq!(short_label(&canon, None), "a folder");
        assert_eq!(
            display(&canon, Some(&root.canonicalize().unwrap())),
            "~/a folder"
        );
        std::fs::remove_dir_all(&root).unwrap();
    }
}
