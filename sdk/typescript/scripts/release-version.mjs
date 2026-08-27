const RELEASE_TAG = /^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-(a|b|rc)\.(0|[1-9]\d*))?$/;

export function versionsFromTag(tag) {
  const match = RELEASE_TAG.exec(String(tag));
  if (!match) {
    throw new Error(
      `invalid release tag ${JSON.stringify(tag)}; expected vMAJOR.MINOR.PATCH` +
      " or vMAJOR.MINOR.PATCH-(a|b|rc).N with no build metadata",
    );
  }
  const base = `${match[1]}.${match[2]}.${match[3]}`;
  return Object.freeze({
    tag,
    npm: match[4] ? `${base}-${match[4]}.${match[5]}` : base,
    python: match[4] ? `${base}${match[4]}${match[5]}` : base,
  });
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const versions = versionsFromTag(process.argv[2]);
  process.stdout.write(`${JSON.stringify(versions)}\n`);
}
