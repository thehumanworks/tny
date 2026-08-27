import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";

const packageRoot = process.env.TNY_SDK_PACKAGE_ROOT
  ? resolve(process.env.TNY_SDK_PACKAGE_ROOT)
  : resolve(new URL("..", import.meta.url).pathname);
const sdk = await import(pathToFileURL(join(packageRoot, "dist/index.mjs")).href);

export const {
  PermissionDecision, Runtime, TnyError, UnsupportedFeatureError, eventKinds,
} = sdk;
export { packageRoot as sdkPackageRoot };
