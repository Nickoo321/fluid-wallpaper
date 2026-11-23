# How to push a change

For people and AI coding agents who changed something in Fluid Wallpaper and want to send it back.
How the code works and how to check a change: [AGENTS.md](AGENTS.md).

## The short version

You cannot push to this repository directly. Only its owner can, and that is on purpose: whatever lands in
`release/` installs itself on every installed copy within a day. So a change travels like this:

1. your own copy of the repository on GitHub (a **fork**),
2. a **branch** in that fork with your change,
3. a **pull request** from that branch to `main` here,
4. the owner looks at it and, if it is accepted, ships it in the next version.

## Once: set up

You need a GitHub account and [Git for Windows](https://git-scm.com/download/win).

1. On <https://github.com/Nickoo321/fluid-wallpaper> press **Fork**. That makes
   `https://github.com/YOUR-NAME/fluid-wallpaper`.
2. Clone your fork and tell git where the original is:

```powershell
git clone https://github.com/YOUR-NAME/fluid-wallpaper.git
cd fluid-wallpaper
git remote add upstream https://github.com/Nickoo321/fluid-wallpaper.git
```

The first push opens a browser window that asks you to sign in to GitHub. A person has to do that once; an AI
agent cannot sign in for you. After it, pushes go through without asking.

## For every change

Start from the newest `main`, on a branch of its own:

```powershell
git fetch upstream
git switch -c my-feature upstream/main
```

Make the change, build it and check it (the checklist is in [AGENTS.md](AGENTS.md), "Before you send it").
Then look at what you are about to send:

```powershell
git status
git diff --stat
```

`git diff --stat` should list only the files you meant to change, with line counts that match the size of the change.
A source file that shows thousands of changed lines was re-saved with other line endings or reformatted: undo that
(`git checkout -- <file>`) and make the edit again. The source files use CRLF line endings and UTF-8.

Commit and push the branch to your fork:

```powershell
git add src tools/installer/data
git commit -m "Droplets: a setting for ring clumping (off by default)"
git push -u origin my-feature
```

Name the folders or files you changed in `git add`; do not use `git add -A`.

## Open the pull request

`git push` prints a link that starts with `https://github.com/YOUR-NAME/fluid-wallpaper/pull/new/`. Open it, check
that the base is `Nickoo321/fluid-wallpaper` `main`, and press **Create pull request**. With the GitHub command line
tool the same thing is:

```powershell
gh pr create --repo Nickoo321/fluid-wallpaper --base main --fill
```

Write in the pull request:

- what the change does, in one or two sentences;
- the name of every new setting, its ini section and key, and its default;
- how you checked it: the commands you ran and what came out (the "feature off" check from AGENTS.md, HDR on and off
  if colour or brightness changed);
- a picture before and after, if the change is something you can see.

One feature per pull request. A small one is looked at sooner.

## Never push these

- **Anything under `release/`**, including `SHA256.txt`. Those files are the published installer and the update
  list; a pull request that touches them is closed.
- Build output (`build/`, `dist/`; they are in `.gitignore`).
- Anything personal: your Windows user name or folder paths, an e-mail address, a password, a key or a token, a log
  file. This repository is public and a pushed commit cannot be taken back cleanly.
- Changes to `src/third_party/imgui/` (it is the library as published).
- Reformatting, renamed variables or moved code that the feature does not need.

## What happens after

The owner reads the pull request and may ask for changes: push more commits to the same branch and the pull request
updates itself.

An accepted change goes into the owner's own working copy of the code first and comes out with the next version in
`release/`. The source in this repository is replaced as a whole with each version, so the pull request is then
either merged or closed with a note that names the version carrying it. From there every installed copy picks the
version up by itself (see "Updates" in the README).

The repository is under the MIT licence; a contribution goes in under the same licence.

## Keep your fork current

Before you start the next change, and whenever a pull request shows conflicts:

```powershell
git fetch upstream
git rebase upstream/main
git push --force-with-lease
```

`--force-with-lease` is for your own feature branch in your own fork only.

## If you only want the feature for yourself

You do not have to send anything. Build it, switch the update off in your installed copy (an empty file named
`no-auto-update.txt` in `%APPDATA%\FluidWallpaper`) and put your exe in place, or run it from the build folder. To
install it on another PC of yours, build your own Setup: "Build the installer" in the README.
