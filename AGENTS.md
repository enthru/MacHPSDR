# Project workflow

The user tests changes on another machine. After completing and verifying a
change, commit the task's changes and push the current branch to its Git remote
without waiting for a separate request. Report the branch and commit pushed.
Follow any explicit instruction from the user to postpone a commit or push.

## Browser remote documentation

Before changing the embedded browser remote, read both
`doc/web-remote.md` (normative product/protocol specification) and
`doc/web-remote-status.md` (implementation record and current handoff).
Update both in the same commit whenever behaviour, protocol, architecture,
verification status, limitations, or the next implementation step changes.
The status document must say exactly what was tested and must not turn an
unverified desktop or on-air claim into a completed one.
