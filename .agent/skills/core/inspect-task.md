# Inspect Task

## Purpose

Convert the user request into a small actionable Wisp task.

## When to use

Use at the beginning of any bugfix, feature, refactor, test, build, docs, or release work.

## Inputs

- User request
- Any error output/logs/screenshots
- Current branch/change context if known

## Read first

- [Router](../../ROUTER.md)
- [Project Summary](../../memory/project-summary.md)
- [Known Issues](../../memory/known-issues.md)

## Steps

1. Classify the task.
2. Identify expected output.
3. List constraints and files that must not be touched.
4. Select one map.
5. Select one primary skill.
6. Write a short plan or ask only for blocking missing facts.

## Output

- Task classification
- Selected map/skill
- Initial constraints
- Verification idea

## Verification

The task can be routed to a specific map and skill without loading unrelated files.

## Next skills

- [Make Plan](make-plan.md)
- [Read Repository](read-repository.md)

## Anti-patterns

- Do not start coding before classification.
- Do not broaden the task.
- Do not assume build commands if commands memory already has them.
