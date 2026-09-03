# Agent instructions

## Validate Capy sources

Use `capyc --check` to validate Capy source without an artifact.

```bash
capyc --check file.capy
capyc --check file.capy other.capy site/
cat source.capy | capyc --check -
```

The command reports one diagnostic per failed file as `file:line:column: message`.
It returns 0 when all files pass. It returns 1 when a file fails and 2 for invalid command arguments.
