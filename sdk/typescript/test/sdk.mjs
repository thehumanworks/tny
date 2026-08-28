import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const packageRoot = process.env.TNY_SDK_PACKAGE_ROOT
  ? resolve(process.env.TNY_SDK_PACKAGE_ROOT)
  : resolve(new URL("..", import.meta.url).pathname);
const sdk = await import(pathToFileURL(join(packageRoot, "dist/index.mjs")).href);
// Resolve the addon the way dist/index.mjs does: a checkout build lives in
// build/Release, a released install in the platform package's prebuilds/.
// Hard-coding build/Release broke package-mode conformance for real tags.
const { resolveNativeAddon } = await import(
  pathToFileURL(join(packageRoot, "scripts/native-loader.mjs")).href
);
const { addonPath } = resolveNativeAddon({ packageRoot });

export const {
  PermissionDecision, Runtime, TnyError, UnsupportedFeatureError, eventKinds,
} = sdk;
export { packageRoot as sdkPackageRoot, addonPath as sdkAddonPath };
