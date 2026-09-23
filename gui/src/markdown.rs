//! Markdown replies as blocks. Lines are grouped by `action` (a finite table,
//! proven and exported by `proofs/Proofs/Markdown.lean`); inline emphasis, code
//! spans and links inside a block are rendered by Slint's `StyledText`, which
//! supports that subset but not headings, quotes, fences, tables or HTML.
//!
//! The Lean model proves, for any line classifier, that the blocks' source
//! lines concatenate back to the reply (nothing dropped, duplicated or
//! reordered) and that an open fence is never re-interpreted. Model output is
//! untrusted display data: HTML is escaped, links are shown but never followed.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LineKind {
    Blank,
    Fence,
    Heading,
    Rule,
    Item,
    Quote,
    Table,
    Text,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockKind {
    Gap,
    Heading,
    Rule,
    Para,
    Item,
    Quote,
    Table,
    Code,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Open {
    None,
    Para,
    Item,
    Quote,
    Table,
    Fence,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Action {
    Append,
    AppendClose,
    Start(BlockKind),
    Single(BlockKind),
}

/// `Markdown.action` in the Lean model.
pub fn action(open: Open, kind: LineKind) -> Action {
    use {Action::*, LineKind as L};
    match (open, kind) {
        // Inside a fence only the closing fence means anything.
        (Open::Fence, L::Fence) => AppendClose,
        (Open::Fence, _) => Append,
        (_, L::Blank) => Single(BlockKind::Gap),
        (_, L::Fence) => Start(BlockKind::Code),
        (_, L::Heading) => Single(BlockKind::Heading),
        (_, L::Rule) => Single(BlockKind::Rule),
        (_, L::Item) => Start(BlockKind::Item),
        (Open::Quote, L::Quote) => Append,
        (_, L::Quote) => Start(BlockKind::Quote),
        (Open::Table, L::Table) => Append,
        (_, L::Table) => Start(BlockKind::Table),
        // Lazy continuation: text joins an open paragraph, list item or quote.
        (Open::Para | Open::Item | Open::Quote, L::Text) => Append,
        (_, L::Text) => Start(BlockKind::Para),
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Block<'a> {
    pub kind: BlockKind,
    pub lines: Vec<&'a str>,
}

fn open_of(cur: Option<&Block>) -> Open {
    match cur.map(|b| b.kind) {
        Some(BlockKind::Para) => Open::Para,
        Some(BlockKind::Item) => Open::Item,
        Some(BlockKind::Quote) => Open::Quote,
        Some(BlockKind::Table) => Open::Table,
        Some(BlockKind::Code) => Open::Fence,
        _ => Open::None,
    }
}

/// Up to three leading spaces, as CommonMark allows before a block marker.
fn block_start(line: &str) -> Option<&str> {
    let rest = line.trim_start_matches(' ');
    (line.len() - rest.len() <= 3).then_some(rest)
}

/// A fence opener's marker character and length.
fn fence_marker(line: &str) -> Option<(char, usize)> {
    let rest = block_start(line)?;
    let marker = rest.chars().next().filter(|c| *c == '`' || *c == '~')?;
    let count = rest.chars().take_while(|c| *c == marker).count();
    // A backtick fence's info string cannot contain backticks.
    (count >= 3 && !(marker == '`' && rest[count..].contains('`'))).then_some((marker, count))
}

fn closes_fence(opener: &str, line: &str) -> bool {
    let (Some((marker, count)), Some(rest)) = (fence_marker(opener), block_start(line)) else {
        return false;
    };
    let run = rest.chars().take_while(|c| *c == marker).count();
    run >= count && rest[run..].trim().is_empty()
}

fn is_rule(rest: &str) -> bool {
    let mut marks = rest.chars().filter(|c| !c.is_whitespace());
    let Some(first) = marks.next().filter(|c| matches!(c, '-' | '*' | '_')) else {
        return false;
    };
    let others: Vec<char> = marks.collect();
    others.len() >= 2 && others.iter().all(|c| *c == first)
}

fn heading_level(rest: &str) -> Option<usize> {
    let level = rest.chars().take_while(|c| *c == '#').count();
    ((1..=6).contains(&level)
        && rest[level..]
            .chars()
            .next()
            .is_none_or(|c| c == ' ' || c == '\t'))
    .then_some(level)
}

/// `(indent, marker, text)` for `- x`, `* x`, `+ x`, `1. x` and `1) x`.
fn item_parts(line: &str) -> Option<(usize, String, &str)> {
    let rest = line.trim_start_matches([' ', '\t']);
    let indent = line.len() - rest.len();
    let digits = rest.chars().take_while(char::is_ascii_digit).count();
    let (marker, after) = if digits == 0 {
        let c = rest
            .chars()
            .next()
            .filter(|c| matches!(c, '-' | '*' | '+'))?;
        (c.to_string(), &rest[1..])
    } else if digits <= 9 && matches!(rest[digits..].chars().next(), Some('.' | ')')) {
        (rest[..=digits].to_string(), &rest[digits + 1..])
    } else {
        return None;
    };
    if !(after.is_empty() || after.starts_with([' ', '\t'])) {
        return None;
    }
    Some((indent, marker, after.trim_start()))
}

/// Classify one line given the open block. Inside a fence only a matching
/// closer is a `Fence`; everything else is `Text` and appended verbatim.
pub fn classify(cur: Option<&Block>, line: &str) -> LineKind {
    if let Some(block) = cur.filter(|b| b.kind == BlockKind::Code) {
        return if closes_fence(block.lines[0], line) {
            LineKind::Fence
        } else {
            LineKind::Text
        };
    }
    if line.trim().is_empty() {
        return LineKind::Blank;
    }
    if fence_marker(line).is_some() {
        return LineKind::Fence;
    }
    if let Some(rest) = block_start(line) {
        if heading_level(rest).is_some() {
            return LineKind::Heading;
        }
        if is_rule(rest) {
            return LineKind::Rule;
        }
        if rest.starts_with('>') {
            return LineKind::Quote;
        }
    }
    if item_parts(line).is_some() {
        return LineKind::Item;
    }
    if line.trim_start().starts_with('|') {
        return LineKind::Table;
    }
    LineKind::Text
}

/// `Markdown.parse` in the Lean model, with the Rust classifier.
pub fn blocks(text: &str) -> Vec<Block<'_>> {
    let mut done = Vec::new();
    let mut cur: Option<Block> = None;
    for line in text.split('\n') {
        match action(open_of(cur.as_ref()), classify(cur.as_ref(), line)) {
            Action::Append => cur
                .as_mut()
                .expect("append only with an open block")
                .lines
                .push(line),
            Action::AppendClose => {
                let mut block = cur.take().expect("close only with an open block");
                block.lines.push(line);
                done.push(block);
            }
            Action::Start(kind) => {
                done.extend(cur.take());
                cur = Some(Block {
                    kind,
                    lines: vec![line],
                });
            }
            Action::Single(kind) => {
                done.extend(cur.take());
                done.push(Block {
                    kind,
                    lines: vec![line],
                });
            }
        }
    }
    done.extend(cur);
    done
}

/// One block ready for the view. `inline` is Markdown for `StyledText`;
/// `plain` is verbatim monospace text (code, tables).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Rendered {
    pub kind: BlockKind,
    pub inline: String,
    pub plain: String,
    pub level: usize,
    pub marker: String,
}

impl Rendered {
    fn new(kind: BlockKind) -> Self {
        Self {
            kind,
            inline: String::new(),
            plain: String::new(),
            level: 0,
            marker: String::new(),
        }
    }
}

/// Escape what `StyledText` would reject or misread, never touching code spans:
/// HTML (`<`), Slint's interpolation placeholder, images, and a leading block
/// marker left on a continuation line.
pub fn inline_source(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 8);
    for (i, line) in text.split('\n').enumerate() {
        if i > 0 {
            out.push('\n');
        }
        let line = line.trim_start();
        if starts_block(line) {
            out.push('\\');
        }
        escape_outside_code(line, &mut out);
    }
    out
}

fn starts_block(line: &str) -> bool {
    let first = line.chars().next();
    matches!(first, Some('#' | '>' | '=' | '|'))
        || item_parts(line).is_some()
        || (first.is_some_and(|c| matches!(c, '-' | '_' | '*')) && is_rule(line))
        || fence_marker(line).is_some()
}

fn escape_outside_code(line: &str, out: &mut String) {
    let bytes = line.as_bytes();
    let mut i = 0;
    while i < line.len() {
        if bytes[i] == b'`' {
            let run = line[i..].bytes().take_while(|b| *b == b'`').count();
            let fence = &line[i..i + run];
            // A code span closes at the next run of exactly the same length.
            let mut search = i + run;
            let mut close = None;
            while let Some(p) = line[search..].find(fence) {
                let at = search + p;
                let len = line[at..].bytes().take_while(|b| *b == b'`').count();
                if len == run {
                    close = Some(at);
                    break;
                }
                search = at + len;
            }
            match close {
                Some(end) => {
                    out.push_str(&line[i..end + run]);
                    i = end + run;
                }
                None => {
                    // An unmatched run is literal text.
                    for _ in 0..run {
                        out.push_str("\\`");
                    }
                    i += run;
                }
            }
            continue;
        }
        let c = line[i..].chars().next().expect("char boundary");
        match c {
            '<' => out.push_str("\\<"),
            '!' if line[i + 1..].starts_with('[') => out.push_str("\\!"),
            '\u{e541}' => out.push('\u{fffd}'),
            _ => out.push(c),
        }
        i += c.len_utf8();
    }
}

fn strip_quote(line: &str) -> &str {
    match block_start(line).and_then(|rest| rest.strip_prefix('>')) {
        Some(rest) => rest.strip_prefix(' ').unwrap_or(rest),
        None => line,
    }
}

fn table_cells(line: &str) -> Vec<String> {
    let line = line.trim();
    let line = line.strip_prefix('|').unwrap_or(line);
    let line = line.strip_suffix('|').unwrap_or(line);
    line.split('|')
        .map(|cell| cell.trim().replace("**", "").replace('`', ""))
        .collect()
}

fn is_separator(cells: &[String]) -> bool {
    !cells.is_empty()
        && cells.iter().all(|c| {
            let c = c.trim_matches(':');
            !c.is_empty() && c.chars().all(|ch| ch == '-')
        })
}

fn render_table(lines: &[&str]) -> String {
    let rows: Vec<Vec<String>> = lines
        .iter()
        .map(|line| table_cells(line))
        .filter(|cells| !is_separator(cells))
        .collect();
    let columns = rows.iter().map(Vec::len).max().unwrap_or(0);
    let widths: Vec<usize> = (0..columns)
        .map(|c| {
            rows.iter()
                .filter_map(|r| r.get(c))
                .map(|cell| cell.chars().count())
                .max()
                .unwrap_or(0)
        })
        .collect();
    rows.iter()
        .map(|row| {
            let cells: Vec<String> = (0..columns)
                .map(|c| {
                    let cell = row.get(c).map_or("", String::as_str);
                    let pad = widths[c].saturating_sub(cell.chars().count());
                    format!("{cell}{}", " ".repeat(pad))
                })
                .collect();
            cells.join("   ").trim_end().to_string()
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// Blocks as the chat view shows them. Gaps are spacing, not content.
pub fn render(text: &str) -> Vec<Rendered> {
    let mut out = Vec::new();
    for block in blocks(text) {
        let mut r = Rendered::new(block.kind);
        match block.kind {
            BlockKind::Gap => continue,
            BlockKind::Rule => {}
            BlockKind::Heading => {
                let rest = block_start(block.lines[0]).unwrap_or(block.lines[0]);
                r.level = heading_level(rest).unwrap_or(1);
                let title = rest[r.level..].trim();
                // A closing run of `#` is decoration, not content.
                let trimmed = title.trim_end_matches('#');
                let title = if trimmed.is_empty() || trimmed.ends_with(' ') {
                    trimmed.trim_end()
                } else {
                    title
                };
                r.inline = inline_source(title);
            }
            BlockKind::Para => r.inline = inline_source(&block.lines.join("\n")),
            BlockKind::Quote => {
                let body: Vec<&str> = block.lines.iter().map(|l| strip_quote(l)).collect();
                r.inline = inline_source(&body.join("\n"));
            }
            BlockKind::Item => {
                let (indent, marker, first) =
                    item_parts(block.lines[0]).unwrap_or((0, "-".into(), block.lines[0]));
                r.level = (indent / 2).min(4);
                r.marker = if marker.ends_with(['.', ')']) {
                    marker
                } else if r.level == 0 {
                    "•".into()
                } else {
                    "◦".into()
                };
                let mut body = vec![first];
                body.extend(block.lines[1..].iter().map(|l| l.trim_start()));
                r.inline = inline_source(&body.join("\n"));
            }
            BlockKind::Table => r.plain = render_table(&block.lines),
            BlockKind::Code => {
                let opener = block.lines[0];
                let closed = block.lines.len() > 1
                    && closes_fence(opener, block.lines[block.lines.len() - 1]);
                let end = if closed {
                    block.lines.len() - 1
                } else {
                    block.lines.len()
                };
                let info = block_start(opener)
                    .unwrap_or(opener)
                    .trim_start_matches(['`', '~'])
                    .trim();
                r.marker = info
                    .split_whitespace()
                    .next()
                    .unwrap_or("")
                    .chars()
                    .take(24)
                    .collect();
                r.plain = block.lines[1..end].join("\n");
            }
        }
        out.push(r);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const KINDS: [(LineKind, &str); 8] = [
        (LineKind::Blank, "blank"),
        (LineKind::Fence, "fence"),
        (LineKind::Heading, "heading"),
        (LineKind::Rule, "rule"),
        (LineKind::Item, "item"),
        (LineKind::Quote, "quote"),
        (LineKind::Table, "table"),
        (LineKind::Text, "text"),
    ];
    const OPENS: [(Open, &str); 6] = [
        (Open::None, "none"),
        (Open::Para, "para"),
        (Open::Item, "item"),
        (Open::Quote, "quote"),
        (Open::Table, "table"),
        (Open::Fence, "fence"),
    ];

    fn block_name(kind: BlockKind) -> &'static str {
        match kind {
            BlockKind::Gap => "gap",
            BlockKind::Heading => "heading",
            BlockKind::Rule => "rule",
            BlockKind::Para => "para",
            BlockKind::Item => "item",
            BlockKind::Quote => "quote",
            BlockKind::Table => "table",
            BlockKind::Code => "code",
        }
    }

    fn action_name(a: Action) -> String {
        match a {
            Action::Append => "append".into(),
            Action::AppendClose => "append_close".into(),
            Action::Start(k) => format!("start:{}", block_name(k)),
            Action::Single(k) => format!("single:{}", block_name(k)),
        }
    }

    #[test]
    fn action_matches_every_row_of_the_proven_lean_table() {
        let table = include_str!("../proofs/golden/markdown.tsv");
        let mut count = 0;
        for line in table.lines().skip(1) {
            let cols: Vec<&str> = line.split('\t').collect();
            let open = OPENS.iter().find(|(_, n)| *n == cols[0]).unwrap().0;
            let kind = KINDS.iter().find(|(_, n)| *n == cols[1]).unwrap().0;
            assert_eq!(action_name(action(open, kind)), cols[2], "{cols:?}");
            count += 1;
        }
        assert_eq!(count, OPENS.len() * KINDS.len());
    }

    /// Deterministic pseudo-random Markdown-ish text (no new dependencies).
    fn sample(seed: &mut u64) -> String {
        const PIECES: [&str; 22] = [
            "```rust",
            "```",
            "~~~",
            "    ```",
            "# Title",
            "### x ###",
            "---",
            "* * *",
            "- item",
            "  - nested",
            "1. first",
            "> quote",
            "| a | b |",
            "|---|---|",
            "plain text",
            "**bold** and `code <T>`",
            "",
            "  ",
            "text with <html>",
            "![img](x.png)",
            "\t- tab",
            "```` inner ``` ````",
        ];
        let mut out = Vec::new();
        for _ in 0..(*seed % 17) {
            *seed = seed
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            out.push(PIECES[(*seed >> 33) as usize % PIECES.len()]);
        }
        out.join("\n")
    }

    #[test]
    fn blocks_round_trip_every_line_like_the_lean_theorem() {
        let mut seed = 7u64;
        for _ in 0..4000 {
            let text = sample(&mut seed);
            let parsed = blocks(&text);
            let source: Vec<&str> = parsed
                .iter()
                .flat_map(|b| b.lines.iter().copied())
                .collect();
            assert_eq!(source.join("\n"), text);
            assert!(parsed.iter().all(|b| !b.lines.is_empty()));
            // Every inline source is accepted by StyledText (or safely falls back).
            for r in render(&text) {
                let _ = styled(&r.inline);
            }
            seed = seed.wrapping_add(1);
        }
    }

    fn styled(src: &str) -> slint::StyledText {
        slint::StyledText::from_markdown(src)
            .unwrap_or_else(|_| slint::StyledText::from_plain_text(src))
    }

    #[test]
    fn renders_the_screenshot_reply_as_text_and_a_code_block() {
        let reply = "Files and directories in the current workdir:\n\n```text\n.git/  .clang-format\nsrc/  tests/\n```";
        let r = render(reply);
        assert_eq!(r.len(), 2);
        assert_eq!(r[0].kind, BlockKind::Para);
        assert_eq!(r[1].kind, BlockKind::Code);
        assert_eq!(r[1].marker, "text");
        assert_eq!(r[1].plain, ".git/  .clang-format\nsrc/  tests/");
    }

    #[test]
    fn an_unclosed_streaming_fence_stays_one_code_block() {
        let r = render("```\n# not a heading\n- not an item");
        assert_eq!(r.len(), 1);
        assert_eq!(r[0].kind, BlockKind::Code);
        assert_eq!(r[0].plain, "# not a heading\n- not an item");
        // A shorter or different fence does not close it.
        let r = render("````\n```\nstill code\n~~~~\n````\nafter");
        assert_eq!(r[0].plain, "```\nstill code\n~~~~");
        assert_eq!(r[1].kind, BlockKind::Para);
    }

    #[test]
    fn headings_lists_quotes_rules_and_tables() {
        let r = render("## Plan ##\n- one\n  continued\n  - nested\n3) three\n> said\nlazy\n\n---\n| a | b |\n|:-|-:|\n| long cell | `x` |");
        let kinds: Vec<BlockKind> = r.iter().map(|b| b.kind).collect();
        assert_eq!(
            kinds,
            [
                BlockKind::Heading,
                BlockKind::Item,
                BlockKind::Item,
                BlockKind::Item,
                BlockKind::Quote,
                BlockKind::Rule,
                BlockKind::Table
            ]
        );
        assert_eq!((r[0].level, r[0].inline.as_str()), (2, "Plan"));
        assert_eq!(
            (r[1].marker.as_str(), r[1].inline.as_str()),
            ("•", "one\ncontinued")
        );
        assert_eq!((r[2].marker.as_str(), r[2].level), ("◦", 1));
        assert_eq!(r[3].marker, "3)");
        assert_eq!(r[4].inline, "said\nlazy");
        assert_eq!(r[6].plain, "a           b\nlong cell   x");
    }

    #[test]
    fn inline_source_escapes_html_and_images_but_not_code_spans() {
        assert_eq!(
            inline_source("use `Vec<T>` not <b>"),
            "use `Vec<T>` not \\<b>"
        );
        assert_eq!(inline_source("see ![x](y)"), "see \\![x](y)");
        assert_eq!(inline_source("a\n# b\n- c"), "a\n\\# b\n\\- c");
        assert_eq!(inline_source("odd ` tick"), "odd \\` tick");
        assert_eq!(inline_source("``a ` b``"), "``a ` b``");
        for src in [
            "x <div>",
            "**bold** _it_ ~~no~~ [link](https://e.x) `c`",
            "a\n> b",
            "1. x\n2. y",
        ] {
            assert!(
                slint::StyledText::from_markdown(&inline_source(src)).is_ok(),
                "{src}"
            );
        }
    }
}
