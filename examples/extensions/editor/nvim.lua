-- Type-check tny tool extensions in Neovim 0.11+ (with nvim-lspconfig's
-- server definitions installed). Put this in your init.lua.
--
-- `import tny` resolves either from an environment where the tny Python
-- package is installed (`pip install -e <tny checkout>/sdk/python`), or, with
-- no install at all, from a checkout's python/ directory via extraPaths.
local tny_python = vim.fn.expand("~/src/tny/python") -- your checkout's python/

vim.lsp.config("basedpyright", {
  settings = {
    basedpyright = {
      analysis = {
        extraPaths = { tny_python },
        typeCheckingMode = "standard",
      },
    },
  },
})
vim.lsp.enable("basedpyright")

-- Plain pyright takes the same setting under `python.analysis`:
-- vim.lsp.config("pyright", {
--   settings = { python = { analysis = { extraPaths = { tny_python } } } },
-- })
-- vim.lsp.enable("pyright")
