# Agent instructions

## Deployment approval

A push to `origin/main` becomes eligible for CT100's nightly deployment. Commit and deployment are not separate approval checkpoints.

To hold deployment, add this exact checklist marker to a tracked file directly under `docs/work/`:

```markdown
- [ ] DEPLOY_APPROVAL: Describe the decision that needs human approval.
```

Leave the marker unchecked until the human approves. After approval, the author checks it and records the approver, date, and scope:

```markdown
- [x] DEPLOY_APPROVAL: Approved by NAME on DATE for SCOPE.
```

Commit and push that release. The updater logs each held or released marker with its revision, file, and line number. Any unchecked marker blocks the entire candidate before a build or service change. Ordinary unchecked tasks and records without this marker do not block deployment. Do not delete or rename a marker to bypass approval. Keep the checked marker as evidence.

The installed updater scans the fetched commit, not uncommitted files. It builds and deploys that same commit even if the remote advances during the run.

## Validate Capy sources

Use `capyc --check` to validate Capy source without an artifact.

```bash
capyc --check file.capy
capyc --check file.capy other.capy site/
cat source.capy | capyc --check -
```

The command reports one diagnostic per failed file as `file:line:column: message`.
It returns 0 when all files pass. It returns 1 when a file fails and 2 for invalid command arguments.
