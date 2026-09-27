//! C ABI adapter for the code-mode benchmark host (bench.h). Benchmark only.
//! `tools` is a sandbox class whose methods call three external functions;
//! `json` is imported by the same prologue. Generated code follows unchanged.
use std::ffi::{CStr, CString, c_char, c_void};
use std::time::Duration;

use monty::{MontyRun, RunProgress};
use monty_types::{CompileOptions, MontyObject, PrintWriter, ResourceLimits, ResourceTracker};

type CallFn = unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> *mut c_char;
type DescribeFn = unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut c_char;

const PROLOGUE: &str = "import json\nclass _TnyTools:\n    def call(self, name, arguments):\n        return __tny_call(name, arguments)\n    def list(self):\n        return __tny_list()\n    def describe(self, name):\n        return __tny_describe(name)\ntools = _TnyTools()\n";

unsafe extern "C" {
    fn free(ptr: *mut c_void);
}

fn take(ptr: *mut c_char) -> Option<String> {
    if ptr.is_null() {
        return None;
    }
    let text = unsafe { CStr::from_ptr(ptr) }.to_string_lossy().into_owned();
    unsafe { free(ptr.cast()) };
    Some(text)
}

/// Runs `code`; writes collected stdout (or an `error: ...` line) to `out`.
/// Returns 1 on success, 0 on a Python/limit error.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn tny_monty_probe_run(
    code: *const c_char,
    catalog: *const c_char,
    call: CallFn,
    describe: DescribeFn,
    ud: *mut c_void,
    timeout_ms: u64,
    max_output: usize,
    out: *mut *mut c_char,
) -> i32 {
    let code = unsafe { CStr::from_ptr(code) }.to_string_lossy().into_owned();
    let catalog = unsafe { CStr::from_ptr(catalog) }.to_string_lossy().into_owned();
    let mut printed = String::new();
    let limits = ResourceLimits {
        max_feed_duration: Some(Duration::from_millis(timeout_ms)),
        ..ResourceLimits::default()
    };
    let source = format!("{PROLOGUE}{code}");
    let names = vec!["__tny_call".into(), "__tny_list".into(), "__tny_describe".into()];
    let inputs = vec![
        MontyObject::function("__tny_call", None),
        MontyObject::function("__tny_list", None),
        MontyObject::function("__tny_describe", None),
    ];
    let result = (|| -> Result<(), String> {
        let runner = MontyRun::new(source, "code.py", names, CompileOptions::default())
            .map_err(|e| e.to_string())?;
        let mut progress = runner
            .start(inputs, ResourceTracker::new(limits), PrintWriter::CollectString(&mut printed, Some(max_output)))
            .map_err(|e| e.to_string())?;
        loop {
            match progress {
                RunProgress::Complete(_) => return Ok(()),
                RunProgress::FunctionCall(fc) => {
                    let arg = |i: usize| fc.args.arg(i).and_then(|a| a.as_str().map(str::to_owned));
                    let value = match fc.function_name.as_str() {
                        "__tny_list" => MontyObject::str(catalog.clone()),
                        "__tny_call" => {
                            let (Some(name), Some(args)) = (arg(0), arg(1)) else {
                                return Err("TypeError: tools.call takes two strings".into());
                            };
                            let name = CString::new(name).map_err(|e| e.to_string())?;
                            let args = CString::new(args).map_err(|e| e.to_string())?;
                            match take(unsafe { call(ud, name.as_ptr(), args.as_ptr()) }) {
                                Some(text) => MontyObject::str(text),
                                None => return Err("tool callback failed".into()),
                            }
                        }
                        "__tny_describe" => {
                            let Some(name) = arg(0) else {
                                return Err("TypeError: tools.describe takes a string".into());
                            };
                            let name = CString::new(name).map_err(|e| e.to_string())?;
                            match take(unsafe { describe(ud, name.as_ptr()) }) {
                                Some(text) => MontyObject::str(text),
                                None => MontyObject::none(),
                            }
                        }
                        other => return Err(format!("unknown external {other}")),
                    };
                    progress = fc
                        .resume(value, PrintWriter::CollectString(&mut printed, Some(max_output)))
                        .map_err(|e| e.to_string())?;
                }
                _ => return Err("unsupported suspension".into()),
            }
        }
    })();
    let ok = result.is_ok();
    if let Err(message) = result {
        printed.push_str("error: ");
        printed.push_str(&message);
    }
    let text = CString::new(printed.replace('\0', "")).unwrap_or_default();
    unsafe { *out = text.into_raw() };
    i32::from(ok)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn tny_monty_probe_free(text: *mut c_char) {
    if !text.is_null() {
        drop(unsafe { CString::from_raw(text) });
    }
}
