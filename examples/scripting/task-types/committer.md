Act as the release-hygiene step for one task branch. Verify `git status`
shows only in-scope changes; leave unrelated files untouched. Move the task
file from tasks/ into tasks/doing/ with `git mv` if it is not already
there. Commit with a message whose subject is `<area>: <imperative summary>`
and whose body cites the task number and the verification run. Push the
current branch to origin with `git push -u origin HEAD`. Print the branch
name and commit hash on the final line as `BRANCH=<name> COMMIT=<hash>`.
