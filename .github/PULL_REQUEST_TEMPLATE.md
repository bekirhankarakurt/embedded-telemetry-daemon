## Summary

<!-- What does this change and why? -->

## Checklist

- [ ] Builds without warnings on GCC and Clang (`-DETD_WERROR=ON`)
- [ ] `ctest` and `scripts/smoke_test.sh` pass; ASan+UBSan / TSan builds are clean
- [ ] cppcheck and clang-tidy pass
- [ ] Tests added or updated
- [ ] README / CHANGELOG updated (wire-format changes bump `wire::kVersion`)
