const TARGETS = Object.freeze({
  "darwin-arm64": Object.freeze({
    triple: "darwin-arm64", platform: "darwin", architecture: "arm64",
    packageName: "@thehumanworks/tny-darwin-arm64", library: "libtny.1.dylib",
  }),
  "linux-x64": Object.freeze({
    triple: "linux-x86_64", platform: "linux", architecture: "x64",
    packageName: "@thehumanworks/tny-linux-x64", library: "libtny.so.1",
  }),
  "linux-arm64": Object.freeze({
    triple: "linux-aarch64", platform: "linux", architecture: "arm64",
    packageName: "@thehumanworks/tny-linux-arm64", library: "libtny.so.1",
  }),
});

export const targets = Object.freeze(Object.values(TARGETS));

export function targetForHost(platform, architecture, glibcVersion) {
  const key = `${platform}-${architecture}`;
  const target = TARGETS[key];
  if (!target || (platform === "linux" && !glibcVersion)) {
    const libc = platform === "linux" && !glibcVersion ? " (musl/unknown libc)" : "";
    throw new Error(
      `unsupported platform ${platform}-${architecture}${libc}; ` +
      "available native packages are darwin-arm64 and glibc linux-x64/linux-arm64",
    );
  }
  return target;
}

export function targetForTriple(triple) {
  const target = targets.find((candidate) => candidate.triple === triple);
  if (!target) {
    throw new Error(
      `unsupported release triple ${triple}; expected ` +
      targets.map((candidate) => candidate.triple).join(", "),
    );
  }
  return target;
}
