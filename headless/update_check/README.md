# Update check and self-update

These files are the PS5 Native App Boilerplate's (`examples/update-check` and
`examples/self-update`, boilerplate commit 2966cac), copied unchanged except
for one thing: `self_update_ps5.c` takes the paths of the helper, of the app's
`param.json` and of the sequence file from `SELF_UPDATE_HELPER_PATH`,
`SELF_UPDATE_PARAM_PATH` and `SELF_UPDATE_SEQUENCE_PATH` when they are defined.
ProsperoEden defines them (`headless/CMakeLists.txt`, `eden_paths.h`,
`headless/update_notice.cpp`) because its folder is not `/app0` once it runs
with filesystem access.

`eden_paths.h` is ProsperoEden's own. The helper the app sends to the
console's payload loader is in `headless/self_update_helper` (the boilerplate's
`examples/self-update-helper`). Three changes there: its Makefile's source
folders; `swap_entries` in `updater.cpp`, which moves aside only the entries
the release replaces (the boilerplate's helper replaces the whole folder;
ProsperoEden's folder can hold the player's own files, `language.txt` or the
`assets/` folder of earlier versions with keys, firmware and games, which an
update must keep); and `mounted_source`, which takes the installed folder from
ShadowMountPlus's `/user/app/<TITLEID>/mount.lnk` (and refuses an image
install, `mount_img.lnk`) before scanning the usual folders.

In the kit itself, `self_update_check` also copies the catalog entry's
`release_notes` and `release_notes_truncated` into the offer (`notes`,
`notes_truncated`), for the update dialog's What's new view.

`github_release.h` and `github_release.c` are ProsperoEden's own: a build that
names a GitHub repository (`EDEN_UPDATE_REPOSITORY`) asks that repository's
newest release instead of the catalog and fills the same offer, so the
download, the helper and the dialog are the kit's, unchanged. Its trust is
TLS and the release's own `SHA256SUMS`, not the catalog's signature (see the
header). For it, `update_check.c` has one more reader,
`update_check_json_array_next` (an array's elements of any type, read once),
`self_update_ps5.c` has `self_update_check_self_from`, which is
`self_update_check_self` when no repository is named, and its `console_fetch`
tells an answer too long for its buffer (`SELF_UPDATE_FETCH_TOO_LARGE`) from a
failed transfer. The host test is `headless/update_check_check.cpp`
(`tools/check-update-check.py`).

For Nightshade's Apps (`headless/apps.h`), which install and update other apps from their own
GitHub releases, the kit has three more things. `github_release_asset_check` finds a release's
file by its name (`PPSA99003.zip`), whatever the tag, with its SHA-256 from the release's
`SHA256SUMS` or else from GitHub's digest of the file, refusing it with neither or when the two
differ; it asks `releases/latest` the way the app's own check does (`fetch_latest`, taken out of
`github_release_check` as it was), but follows a renamed repository's redirect only to the same
owner (`GITHUB_RELEASE_OWNER_CHANGED` otherwise). `self_update.c` sends an offer with `beside` set as an
`install` request (`self_update_protocol.h`), goes on to the helper's go-ahead by itself once it
is staged and ends in `SELF_UPDATE_INSTALLED`; an offer without it is the kit's update, as
before. And `self_update_ps5.c` sends any payload from the app's folder
(`self_update_open_payload`), the launch payload as well as the helper, with a shorter wait for its
answer when asked (`self_update_channel_timeout`), and `console_download` keeps the address it
follows per call instead of in one static buffer, since downloads can now run at once.

For the bridge (`headless/bridge`, the PPSA99008 app of the release ZIP), `github_release_tag_asset_check`
is `github_release_asset_check` of the release a tag names (`releases/tags/<tag>`) instead of the
newest: the bridge installs Nightshade from the release it came in. That release may still be a
pre-release, its answer must name that tag, and no redirect is followed.
