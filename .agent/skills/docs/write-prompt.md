# Write Prompt

## Purpose

Create or update reusable Open Code prompts for Wisp work.

## When to use

Use when agent workflow should be reusable.

## Inputs

- Task type
- Workflow
- Constraints

## Read first

- [Prompts Directory](../../prompts/task-intake.md)
- [Context Policy](../../CONTEXT_POLICY.md)
- [AGENTS](../../AGENTS.md)

## Steps

1. State role and task type.
2. Reference router, maps, skills, memory.
3. Include Wisp constraints.
4. Keep prompt short and reusable.
5. Avoid embedding large code or docs.

## Output

- Reusable prompt
- Related links

## Verification

Prompt routes future agents without loading excessive context.

## Next skills

- [Write Handoff](../core/write-handoff.md)

## Anti-patterns

- Do not paste entire project context.
- Do not hardcode stale facts.
- Do not tell agents to scan everything.
