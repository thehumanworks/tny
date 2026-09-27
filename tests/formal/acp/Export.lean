import Acp

open Tny.Acp

/-- Empty TSV cells represent missing handshake fields. No escaping is
needed: fixtures contain no tabs/newlines; spaces are deliberately preserved. -/
def fieldOption (s : String) : Option String := if s.isEmpty then none else some s

def bit (b : Bool) : String := if b then "1" else "0"

def versions : List (String × String) :=
  (["-0.75.1", "0.74.4294967295", "0.75.0", "0.75.1", "0.75.2", "+0.75.1",
    "0.81.2", "0.100.0", "0.9.99", "0.75.10",
    "1.0.0", "4294967295.4294967295.4294967295", "0.75.1+001", "1.0.0+abc-XYZ.00",
    "0.75.0+new", "0.75.1-rc.1", "1.0.0-rc.1+build", "00.75.1", "0.075.1",
    "0.75.01", "v0.75.1", " 0.75.1", "0.75.1 ", "0.75", "0.75.1.2",
    "0.75.1+", "0.75.1+a..b", "0.75.1+a+b", "0.75.1+é", "4294967296.0.0",
    "0.4294967296.1", "0.75.4294967296", ""].map (identity, ·)) ++
    [("@agentclientprotocol/claude-agent-acp-other", "0.75.1")]

structure Transition where
  name : String := identity
  version : String := "0.75.1"
  protocol : Nat := 1
  load : Bool := false
  resume : Bool := false

def transitions : List Transition :=
  [{}, {load := true}, {resume := true}, {load := true, resume := true},
   {version := "0.75.0"}, {version := "0.75.0", load := true, resume := true},
   {version := "1.0.0+001", load := true, resume := true},
   {name := "other"}, {name := ""}, {version := ""}, {protocol := 2},
   {version := "0.75.1-rc.1", load := true, resume := true},
   {version := "0.81.2"}, {version := "0.81.2", load := true, resume := true},
   {version := "0.100.0+mise.1", load := true, resume := true},
   {name := "other", version := "0.81.2", load := true, resume := true},
   {version := "0.81.2", protocol := 2, load := true, resume := true},
   {version := "0.81.2", resume := true}]

def versionTable : String :=
  "name\tversion\taccepted\n" ++ String.join (versions.map fun (n, v) =>
    n ++ "\t" ++ v ++ "\t" ++ bit (admit (fieldOption n) (fieldOption v)) ++ "\n")

def transitionTable : String :=
  "name\tversion\tprotocol\tload\tresume\taccepted\n" ++
  String.join (transitions.map fun t =>
    t.name ++ "\t" ++ t.version ++ "\t" ++ toString t.protocol ++ "\t" ++
    bit t.load ++ "\t" ++ bit t.resume ++ "\t" ++
    bit (handshake (fieldOption t.name) (fieldOption t.version) t.protocol t.load t.resume) ++ "\n")

def main (args : List String) : IO Unit := do
  let dir : System.FilePath := args.headD "golden"
  IO.FS.createDirAll dir
  IO.FS.writeFile (dir / "versions.tsv") versionTable
  IO.FS.writeFile (dir / "transitions.tsv") transitionTable
