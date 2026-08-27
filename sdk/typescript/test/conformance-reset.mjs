import { rmSync } from "node:fs";
import { resultDir } from "./result.mjs";

rmSync(resultDir, { recursive: true, force: true });
