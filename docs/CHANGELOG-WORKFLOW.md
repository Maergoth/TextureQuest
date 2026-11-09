# Keeping the changelog current

Root `CHANGELOG.md` is the single canonical history for GitHub and the bundled
installer **What's new** window. Do not maintain a separate UI history copy.

Every app change needs an entry under its app version. Use an exact Markdown
H2 version heading, such as `## 1.13.13`, optionally followed by a title. Keep
the newest version first and match `APP_VERSION` in `src/texturequest_version.py`.
The normal installer build requires the current version entry, bundles the
canonical file and records its hash. Update the entry before that build.

Describe the concrete behavior in a few bullets. Include UI and packaging
changes as well as fixes. State whether native payloads or texture data changed;
the app version and texture version are separate. Record meaningful limitations
and the scope of validation without presenting controlled tests as live visual
approval or game FPS measurements.

Label Develop work as development work. A source push is not a binary release;
do not invent publication dates, uploaded assets or public release status.
Keep README and release notes as short summaries linking to `CHANGELOG.md`.
