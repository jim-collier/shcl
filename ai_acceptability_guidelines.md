<!-- markdownlint-disable MD007 -- Unordered list indentation -->
<!-- markdownlint-disable MD010 -- No hard tabs -->
<!-- markdownlint-disable MD033 -- No inline html -->
<!-- markdownlint-disable MD055 -- Table pipe style [Expected: leading_and_trailing; Actual: leading_only; Missing trailing pipe] -->
<!-- markdownlint-disable MD041 -- First line in a file should be a top-level heading -->

<!-- TOC ignore:true -->
# AI acceptability guidelines

Where AI is allowed near this project, where it isn't, and who is accountable either way.

<!-- TOC ignore:true -->
## Table of contents

<!-- TOC -->

- [Introduction](#introduction)
- [Problems with AI](#problems-with-ai)
	- [Environmental](#environmental)
	- [Economic](#economic)
	- [Ethical](#ethical)
	- [Code quality](#code-quality)
	- [Security risks](#security-risks)
	- [Bad PR](#bad-pr)
- [Non-problems with AI](#non-problems-with-ai)
- [Good uses of AI](#good-uses-of-ai)
	- [Review and analysis](#review-and-analysis)
	- [Tests and tooling](#tests-and-tooling)
	- [Hard problems](#hard-problems)
	- [Porting to other languages](#porting-to-other-languages)
	- [Porting to other operating systems](#porting-to-other-operating-systems)
	- [Tedious non-coding tasks that pay nothing](#tedious-non-coding-tasks-that-pay-nothing)
- [Rules for AI use in this project](#rules-for-ai-use-in-this-project)
	- [A person is accountable for every merged line](#a-person-is-accountable-for-every-merged-line)
	- [Self-assessment of AI speedup is not evidence](#self-assessment-of-ai-speedup-is-not-evidence)
	- [What AI may do with light review](#what-ai-may-do-with-light-review)
	- [What always needs full human review](#what-always-needs-full-human-review)
	- [What AI does not decide](#what-ai-does-not-decide)
	- [Contributing](#contributing)
- [Where this could change](#where-this-could-change)
- [The use of AI in writing this document](#the-use-of-ai-in-writing-this-document)

<!-- /TOC -->

## Introduction

This project's original author has decades of programming experience. Hobby, then professional, then hobby again.

When AI started becoming a "thing" in programming, I (the original author) was against it. Which was convenient, because it sucked at it.

That view has softened with the emergence of frontier models like Claude Fable. (Again convenient now that it's much better.) I've been running AI-led experiments under an alt GitHub account, mostly to find where these tools break and where they hold up. One result is that this nearly decade-old main account no longer has a blanket "no-AI" policy.

This document is a first pass at documenting what, when, and how AI is allowed into this and other projects.

## Problems with AI

Feel free to skip this section. It's here mostly for the record: an acknowledgment of the main society-level problems most people are well aware of.

### Environmental

This is the hardest one to justify.

The scale is not in dispute. US data centers used about 4.4% of the country's electricity in 2023, and [Berkeley Lab](https://newscenter.lbl.gov/2025/01/15/berkeley-lab-report-evaluates-increase-in-electricity-demand-from-data-centers/) puts 2028 somewhere between 6.7% and 12%.

The argument is not that this is fine. It is that cost per unit of useful work is falling faster than the headlines suggest:

- Querying a model at GPT-3.5 quality fell from $20 to $0.07 per million tokens between late 2022 and late 2024, per the [Stanford AI Index](https://hai.stanford.edu/ai-index/2025-ai-index-report). Roughly 280x in under two years.

- Hardware cost per unit of performance is dropping about 30% a year. Energy efficiency is improving about 40% a year.

- Open-weight models have nearly caught up. The benchmark gap against closed models narrowed from 8% to 1.7% in a single year.

The part that matters here: open-weight coding models that fit in 24 to 32 GB on a consumer GPU now score around 80% on SWE-bench Verified, against roughly 90 to 95% for the best hosted models. That is already enough for the review and testing work described below.

On capability, the number usually quoted is [METR's](https://metr.org/blog/2025-03-19-measuring-ai-ability-to-complete-long-tasks/): the length of task a model finishes with 50% reliability has doubled about every seven months over the six years to 2025. Fitting only the 2024-2025 data gives a steeper curve. METR puts no single number on that, but on SWE-bench Verified alone they measured a doubling time under three months.

At a seven-month doubling interval, 3 years is about 35x compounded. At a four-month interval, 3 years is over 500x. That's a trend, not a "prediction".

So the position is narrower than "AI is worth it". It is that the useful capability is on track to run on a desktop with no cloud data center behind it (e.g. in a solar-powered home office), sooner than later.

### Economic

The companies leading this are among the largest ever to exist, and may be steering the global economy toward a cliff. If that happens, the cost will fall mostly on people who never opted in.

Time will tell whether this is the largest bubble in history or whether the anticipated returns arrive first. Either way, one thing seems reasonably likely: As with the railroad and dotcom booms before it, there are arguably "four clocks" we're working with - as a simplified model: 1) The tech clock - the reference clock. How fast the tech progresses. 2) The investment clock - the fastest clock of all in any exuberance boom. 3) The actual adoption clock. 4) The profitability clock - the slowest of the four. Regular people eventually benefited after the railroad and dotcom busts [once all the incredibly excessive rail and fiber buildout eventually started getting used at rock-bottom prices]. The delta between clocks #2 and #4 makes a few people insanely rich, but most late-boom investors lose it all. [In part because it's statistically unlikely to pick the ultimate winners, and in part because they can't stay solvent for a decade or so for the winning returns to catch back up after the bust and finally make them rich.] Their loss is usually the world's eventual gain. Most early innovators that seem like the sure bets don't actually survive the boom/bust/growth cycle. (This played out in both the railroad and dotcom eras. For example, Google entered search years after the presumed winners, and stayed private through the whole boom. Nobody's 1999 sure-bet list in that space survived intact.)

Either way, if guidelines like the ones below were adopted broadly (narrow scope, human accountability, no AI making the decisions that matter) and especially limited to near-future desktop models - then global demand might look less like a gold rush. That could be cope. It may also be true.

### Ethical

These are mostly downstream of the previous two.

The clearest measured harm so far is to entry-level work. Stanford's [Canaries in the Coal Mine](https://siepr.stanford.edu/publications/working-paper/canaries-coal-mine-six-facts-about-recent-employment-effects-artificial) found a 13% relative decline in employment for workers aged 22 to 25 in the most AI-exposed occupations, including software development, while employment for more experienced workers in those same occupations held steady.

That isn't AI's fault. It's decisions made by employers with AI used as a rationale, possibly against their own long-term interests. A profession that stops training juniors runs out of seniors.

This project can't fix that. What it can do is not pretend the problem is imaginary.

### Code quality

Code quality was a serious and universal problem until recently, even on small projects. On current frontier models it still has to be managed.

The main risk is not that the code fails to work. It's that it works and quietly rots. [GitClear's analysis](https://www.gitclear.com/ai_assistant_code_quality_2025_research) of 211 million changed lines found 2024 was the first year on record where copy-pasted code exceeded moved code, with code clones up roughly fourfold. Refactoring went from about a quarter of changed lines in 2021 to under a tenth in 2024.

That is the failure mode to watch: a model asked for a fix writes a new version rather than finding the existing one. Nothing breaks. The codebase just gets worse in a way no test catches.

### Security risks

Running an agentic tool on a development machine is a risk to the developer. It reads local files, runs commands, and reaches the network. Know the risks and act accordingly.

The generated code is another risk. Veracode's [2025 report](https://www.veracode.com/resources/analyst-reports/2025-genai-code-security-report/) found that across 80 tasks and 100+ models, 45% of samples introduced an OWASP Top 10 vulnerability. Java was worst at over 70%. Cross-site scripting was missed in 86% of the cases where it applied.

Hallucinated dependencies are worse. A [study of 576,000 generated samples](https://www.usenix.org/system/files/conference/usenixsecurity25/sec25cycle1-prepub-742-spracklen.pdf) found package names that do not exist in about 5% of commercial-model output and about 22% of open-model output. Attackers register the common ones and wait. Every dependency an AI suggests must be human-verified.

Used the other way around, for adversarial review, security review, and fuzz and regression suites, the same tools measurably improve a project. That asymmetry is a driving factor behind these guidelines.

### Bad PR

Any project accepting AI-generated contributions carries this risk, including this one.

AI is becoming a public enemy. The cause is probably mostly greed-driven hype, the economics, and the tactics used to foist data centers onto communities whose citizens pay the externalities.

That perception may shift once desktop-class open models are good enough to work offline, which looks like a short wait rather than a long one.

Either way, hiding the involvement of AI is not the way out of its growing PR problem. Being transparent about its use, managing it as a tool, and accepting whatever criticism follows is the path taken here.

## Non-problems with AI

The common objection "LLMs don't understand context" is confused at best. Contextualization isn't something bolted onto an LLM. It *is* the mechanism.

The objection usually means something narrower: *situational* context. Who you are, what you ate this morning, what's really at stake. That's a fair criticism, just poorly worded. Where LLMs exceed us is textual context: conditioning on a hundred thousand things at once, across more domains than any one person could read, let alone remember.

And "understand" belongs in quotes, because nobody can specify what understanding *is* beyond what it *does*. We grant it to other humans on inference alone, for free. (But in the end, can we really be sure *any* intelligence - yours, mine, the pilot of your next flight - is anything more than next-word-prediction machines, running on wetware, that got good enough at not dying to mistake itself for something else?)

Whether an eventual AGI is LLM-based is an open question. But LLMs have two advantages now: they can "communicate" with us, and we can literally watch them "think" in our own native language. As for coding, the inventors of computer languages borrowed the machinery of linguistics, which is what LLMs happen to be built for, and that is a large part of why these models got good at code before most other technical work. Even if AI advances beyond LLMs, there may still be a role for LLMs in A) the human interface, and/or B) coding agents.

## Good uses of AI

What current LLMs demonstrably do well:

- Hold far more context at once than a person can. A model reads an entire repository in one pass. Human working memory holds roughly four things.

- Combine ideas across fields that no single reviewer has read, or could in a lifetime.

- Produce solutions that do not appear in their training data. (Contrary to popular misunderstanding.)

- Look up information they were never trained on.

What they cannot do:

- Retain anything between sessions. (Without memory files and even then imperfectly.)

- Exist as one continuous mind.

The rules below are built, in part, with those limits in mind.

### Review and analysis

Verification is where LLMs are strongest, because a wrong answer is cheaper than a defect.

- Adversarial code review. Large context windows trace more code paths and hold more in mind at once than a person can. Hallucination doesn't appear to be an issue here, and models don't get bored.

- Security review: input handling, allowlists, deserialization, anything touching a file path or a URL scheme.

- Performance review. Reading hot paths for repeated work, redundant allocation, and locks held longer than they need to be.

- Triaging linter, static analyzer, and profiler output. These produce more findings than anyone can read. Sorting the real ones from the noise, with reasons attached, is a good fit.

A model that reports a defect can usually write the fix too. Those patches are accepted on the same terms as any other: read, understood, covered by a test.

### Tests and tooling

- Regression suites, especially the pinning kind written after a bug is understood, to prove it stays fixed.

- Fuzz harnesses. Tedious to write, and easy to check.

- CI/CD and build pipelines. Tedious and error-prone, with immediate feedback and verifiable by running.

### Hard problems

- Bugs needing more context than one person can hold at once, where the cause is spread across several files and a dozen interacting conditions. That is a genuine human/AI delta, not just a speed increase.

- Math, algorithms, and logic that have published academic literature behind them. The model knows the papers exist, including obscure historical ones, has read them, and can say whether they apply to the problem at hand. Not necessarily better than a human, just much faster at the research.

### Porting to other languages

Porting a well-defined, documented codebase with comprehensive existing test harnesses to another language is something current frontier models do with high fidelity, including refactoring to target-language idioms.

It is then a fully human responsibility to:

- Read and understand the generated code.

- Insure the code conforms to house style. (Which should be at the linting and autoformatting stage but still needs eyeballs.)

- Make sure it passes all automated *and* manual unit, integration, usability, UAT, regression, performance, and security tests.

### Porting to other operating systems

Similar to the previous point. For some languages (Go, Rust, Zig, and non-compiled cross-platform scripting languages) this is a trivial non-AI task that should just be part of the CI pipeline.

But depending on what the program does, there is often OS-specific branching for functionality the language's own standard library doesn't cover. Models are usually good at "knowing" the idiomatic way to handle those cases.

Human responsibility picks back up at the end, same as with the previous section.

### Tedious non-coding tasks that pay nothing

Examples:

- Demo gif and video generation, including fully anonymized synthetic scenarios. Tedious for humans, and generally "not fun" for either technical or creative types.

- Benchmarking and measurements for competitive comparison charts.

- Asset generation. A tougher call, since creatives need work too and are being replaced by AI at heartbreaking levels. But on a FLOSS project with no pay and nobody stepping up to volunteer, what are you going to do? For example, this author is "artistic enough" - and experienced enough with the tools - to generate image, audio, and video assets by hand. It's just time I'd rather spend on product design, problem-solving, and coding. For assets I usually know exactly what I want and can describe it precisely.

- Boring "required" website setup and generation. Not for the site that *is* the product, where designers and engineers and stakeholders come together to make something good. I mean the bare-minimum commodity web presence even basic FLOSS products need, that nobody wants to slog through unpaid.

## Rules for AI use in this project

### A person is accountable for every merged line

AI is not an author and not a defense. Whoever merges a change owns it, answers questions about it, and fixes it when it breaks. "The model wrote it" is not something anyone gets to say.

The practical test: anyone who cannot explain a change does not merge it.

### Self-assessment of AI speedup is not evidence

METR ran a [randomized trial](https://metr.org/blog/2025-07-10-early-2025-ai-experienced-os-dev-study/) with experienced open-source developers on repositories they already knew well. They were 19% slower with AI tools. They believed they had been 20% faster.

That gap is the important part. Perceived productivity is not measurable by the person experiencing it, so "it's faster this way" carries no weight on its own. Where speed matters, measure it.

That trial ran on early-2025 tools, and METR now flags it as out of date. Their [February 2026 follow-up](https://metr.org/blog/2026-02-24-uplift-update/) on late-2025 tools estimates a speedup instead: about 18% for returning participants and 4% for new ones. The confidence intervals straddle zero in both cases, and the authors warn of heavy selection bias, since developers increasingly refused to participate without AI.

So the direction has moved. The lesson about self-report has not.

### What AI may do with light review

- Read and summarize existing code.

- Review a diff and report findings.

- Draft tests, build scripts, and CI configuration.

- Sort and explain linter, analyzer, and profiler output.

### What always needs full human review

Everything that reaches the repository. Specifically:

- Any change to the security boundary. Scheme allowlists, path handling, deserialization, anything spawning a process.

- Any new dependency. Confirm it exists, is the package intended, and is actually maintained.

- Any generated test. A test that asserts current behavior locks in current bugs, and reads as passing coverage while proving nothing.

- Any change described as a refactor. This is where duplication gets introduced.

### What AI does not decide

- Architecture, and anything that will be expensive to reverse. AI can suggest options, and be asked for them. It doesn't pick.

- What gets released and what gets held back.

- Anything requiring judgment about users rather than about code.

Explaining a tradeoff is useful. Choosing it is not delegated.

### Contributing

Contributions that used AI will not be automatically rejected. Two conditions:

- Say so in the pull request, along with roughly what it was used for. No process detail needed, no apology expected.

- Submit it as work that is understood, tested, and stood behind.

Commit messages describe the change, not the tooling used to make it. The same convention that applies to editors, formatters, and everything else.

## Where this could change

The position stated here is conditional. Things that would tighten it:

- Evidence that these review uses cost more in defects than they catch.

- Contribution volume that makes the review load unsustainable.

Things that would relax it:

- Desktop-class open models that close the remaining gap on agentic work.

- A power story that does not require a hyperscale data center.

The second list is closer than it looks. That is most of why this document exists rather than a shorter one saying no.

## The use of AI in writing this document

Everyone who knows me knows I love to write. *A lot*. Especially about highly technical subjects.

AI was used on this document for:

- **Spell-checking**. I normally use LibreOffice Writer, but it complains about every part of every URL and Markdown link, and my exceptions library is enormous by now. AI knows what to skip.

- **Fact-checking**. Several claims here were overstated at best, or flat-out wrong at worst, and got backed off or removed. And sometimes I learn new things along the way.

- **Reducing "conclusion shopping" and confirmation bias**. We all shop for studies and links that support a preconceived argument. I'm no different. This is a good check on that habit, and probably the single best thing a solo writer can use AI for.

What AI was *not* used for:

- **Content generation**. Every questionable and/or redundant argument made here, every odd injection of unsolicited opinion and narration into what should be a straightforward "guidelines" document, is from a human. This human.

- **Grammar-checking**. I prefer the organic feel of my own tedious phrasing, run-on sentences, and abruptly ending such run-on sentences where I've run out of examples but want it to *seem* like there's more, with ", etc.". If it's not tedious for me to read my own writing, it just doesn't *feel* right.

- **Tone and appropriateness policing**. Again: probably would have been a good idea.
