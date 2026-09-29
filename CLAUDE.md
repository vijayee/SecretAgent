# SecretAgent Project

Always read and follow the coding conventions in [STYLE_GUIDE.md](./docs/STYLE_GUIDE.md) when
writing or modifying C code in this project.

## No TODOs in Completed Work

Never leave a TODO/FIXME/HACK/XXX comment in code and mark a task as completed. Every TODO
represents unfinished work. When completing a task:
- Implement the code the TODO describes, or
- If it requires design decisions beyond scope, escalate to the user rather than leaving it
- A task is not done until every TODO in the files it touched is resolved

## Git Commit Conventions

- **Do NOT add "Co-Authored-By" lines to commit messages.** All commits should have only the
  author's information.
- Use clear, descriptive commit messages following conventional commit format
  (e.g., "feat:", "fix:", "docs:", "test:", "refactor:", "chore:")
- Keep commits focused and atomic - one logical change per commit

## Testing

- Tests are GoogleTest (C++17) with C headers wrapped in `extern "C"`, mirroring module names:
  `test/test_<module>.cpp`
- After any implementation, run `ctest --test-dir cmake-build-debug --output-on-failure` and
  verify no memory leaks (valgrind on the test binary) before marking work done.
- Sanitizer builds are opt-in: `-DSA_ENABLE_ASAN=ON`, `-DSA_ENABLE_UBSAN=ON`,
  `-DSA_ENABLE_TSAN=ON` (TSan and ASan are mutually exclusive). NOTE: ASan binaries on this
  machine may randomly abort in __asan_init due to kernel ASLR entropy — run sanitizer suites
  under `setarch -R ctest ...`.

## Key Patterns

- Reference-counted structs have `refcounter_t refcounter` as the first member
- Types use `_t` suffix, functions follow `module_action()` naming
- Create functions use `get_clear_memory()` and call `refcounter_init()` last
- Actor structs embed `actor_t` as the FIRST member, and dispatch is `void dispatch(void* state, message_t* msg)`
- Never touch the symlinked `liboffs/`, `WaveDB/`, `deepseek-harness/`, `onyx/`, `prime-agent/`,
  `claude-code-source-code/` directories — they are read-only reference codebases