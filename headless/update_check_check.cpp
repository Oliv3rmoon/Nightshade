// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the update check's decision with this app's version numbers
// (update_check/update_check.h, built without its network side), and of a build that asks a
// GitHub repository's releases instead (update_check/github_release.h), through a stand-in for
// the network: the release's answer, its tag's version, SHA256SUMS, the notes, the decision, the
// addresses a download may be made from, and that a build naming no repository asks the catalog
// as before.
#include "update_check/github_release.h"
#include "update_check/self_update.h"
#include "update_check/update_check.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
update_check_result Decide(const std::string& json, const char* installed) {
    update_check_result result{};
    update_check_evaluate(json.data(), json.size(), installed, &result);
    return result;
}

std::string Answer(const char* status, const char* content_version, const char* version) {
    std::string json = std::string("{\"title_id\":\"PPSA99008\",\"status\":\"") + status + "\",\"content_version\":";
    json += content_version ? std::string("\"") + content_version + "\"" : std::string("null");
    json += std::string(",\"version\":\"") + version + "\",\"page\":\"https://homebrew.page/ps5/prosperoeden\"}";
    return json;
}

// releases/latest as GitHub answers it for a release made by tools/ci/make-dist.py and the release
// workflow (the shape of a real answer, pretty-printed; Compact() gives it as GitHub sends it).
const std::string kRelease = R"json({
  "url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/416227100",
  "assets_url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/416227100/assets",
  "upload_url": "https://uploads.github.com/repos/Oliv3rmoon/Nightshade/releases/416227100/assets{?name,label}",
  "html_url": "https://github.com/Oliv3rmoon/Nightshade/releases/tag/v1.005.100",
  "id": 416227100,
  "author": {
    "login": "github-actions[bot]",
    "id": 41898282,
    "node_id": "MDM6Qm90NDE4OTgyODI=",
    "avatar_url": "https://avatars.githubusercontent.com/in/15368?v=4",
    "gravatar_id": "",
    "url": "https://api.github.com/users/github-actions%5Bbot%5D",
    "html_url": "https://github.com/apps/github-actions",
    "followers_url": "https://api.github.com/users/github-actions%5Bbot%5D/followers",
    "following_url": "https://api.github.com/users/github-actions%5Bbot%5D/following{/other_user}",
    "gists_url": "https://api.github.com/users/github-actions%5Bbot%5D/gists{/gist_id}",
    "starred_url": "https://api.github.com/users/github-actions%5Bbot%5D/starred{/owner}{/repo}",
    "subscriptions_url": "https://api.github.com/users/github-actions%5Bbot%5D/subscriptions",
    "organizations_url": "https://api.github.com/users/github-actions%5Bbot%5D/orgs",
    "repos_url": "https://api.github.com/users/github-actions%5Bbot%5D/repos",
    "events_url": "https://api.github.com/users/github-actions%5Bbot%5D/events{/privacy}",
    "received_events_url": "https://api.github.com/users/github-actions%5Bbot%5D/received_events",
    "type": "Bot",
    "user_view_type": "public",
    "site_admin": false
  },
  "node_id": "RE_kwDOU0H7ic4Yzx8c",
  "tag_name": "v1.005.100",
  "target_commitish": "dev/build-3",
  "name": "Nightshade v1.005.100",
  "draft": false,
  "immutable": false,
  "prerelease": false,
  "created_at": "2026-10-25T03:56:24Z",
  "updated_at": "2026-10-25T04:17:04Z",
  "published_at": "2026-10-25T04:17:04Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/assets/652447821",
      "id": 652447821,
      "node_id": "RA_kwDOU0H7ic4m45BN",
      "name": "Nightshade-v1.005.100.zip",
      "label": "",
      "uploader": {
        "login": "github-actions[bot]",
        "id": 41898282,
        "node_id": "MDM6Qm90NDE4OTgyODI=",
        "avatar_url": "https://avatars.githubusercontent.com/in/15368?v=4",
        "gravatar_id": "",
        "url": "https://api.github.com/users/github-actions%5Bbot%5D",
        "html_url": "https://github.com/apps/github-actions",
        "followers_url": "https://api.github.com/users/github-actions%5Bbot%5D/followers",
        "following_url": "https://api.github.com/users/github-actions%5Bbot%5D/following{/other_user}",
        "gists_url": "https://api.github.com/users/github-actions%5Bbot%5D/gists{/gist_id}",
        "starred_url": "https://api.github.com/users/github-actions%5Bbot%5D/starred{/owner}{/repo}",
        "subscriptions_url": "https://api.github.com/users/github-actions%5Bbot%5D/subscriptions",
        "organizations_url": "https://api.github.com/users/github-actions%5Bbot%5D/orgs",
        "repos_url": "https://api.github.com/users/github-actions%5Bbot%5D/repos",
        "events_url": "https://api.github.com/users/github-actions%5Bbot%5D/events{/privacy}",
        "received_events_url": "https://api.github.com/users/github-actions%5Bbot%5D/received_events",
        "type": "Bot",
        "user_view_type": "public",
        "site_admin": false
      },
      "content_type": "application/zip",
      "state": "uploaded",
      "size": 42651073,
      "digest": "sha256:9a726c3a4238bfc9651a8c2bd3952cf07972495a3f930cc6d0a5b487140a5991",
      "download_count": 3,
      "created_at": "2026-10-25T04:17:02Z",
      "updated_at": "2026-10-25T04:17:03Z",
      "browser_download_url": "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.005.100/Nightshade-v1.005.100.zip"
    },
    {
      "url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/assets/652447818",
      "id": 652447818,
      "node_id": "RA_kwDOU0H7ic4m45BK",
      "name": "SHA256SUMS",
      "label": "",
      "uploader": {
        "login": "github-actions[bot]",
        "id": 41898282,
        "node_id": "MDM6Qm90NDE4OTgyODI=",
        "avatar_url": "https://avatars.githubusercontent.com/in/15368?v=4",
        "gravatar_id": "",
        "url": "https://api.github.com/users/github-actions%5Bbot%5D",
        "html_url": "https://github.com/apps/github-actions",
        "followers_url": "https://api.github.com/users/github-actions%5Bbot%5D/followers",
        "following_url": "https://api.github.com/users/github-actions%5Bbot%5D/following{/other_user}",
        "gists_url": "https://api.github.com/users/github-actions%5Bbot%5D/gists{/gist_id}",
        "starred_url": "https://api.github.com/users/github-actions%5Bbot%5D/starred{/owner}{/repo}",
        "subscriptions_url": "https://api.github.com/users/github-actions%5Bbot%5D/subscriptions",
        "organizations_url": "https://api.github.com/users/github-actions%5Bbot%5D/orgs",
        "repos_url": "https://api.github.com/users/github-actions%5Bbot%5D/repos",
        "events_url": "https://api.github.com/users/github-actions%5Bbot%5D/events{/privacy}",
        "received_events_url": "https://api.github.com/users/github-actions%5Bbot%5D/received_events",
        "type": "Bot",
        "user_view_type": "public",
        "site_admin": false
      },
      "content_type": "application/octet-stream",
      "state": "uploaded",
      "size": 94,
      "digest": "sha256:01826a329cf397d61baad969922f1eee753968df68c004eacf7991b4d38e39ca",
      "download_count": 3,
      "created_at": "2026-10-25T04:17:02Z",
      "updated_at": "2026-10-25T04:17:02Z",
      "browser_download_url": "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.005.100/SHA256SUMS"
    }
  ],
  "tarball_url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/tarball/v1.005.100",
  "zipball_url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/zipball/v1.005.100",
  "body": "**Nightshade v1.005.100.** Oliv3rmoon's ProsperoEden, with fixes that aren't in an official release yet.\r\n\r\n## New in 1.005.100\r\n\r\n**Graphics driver**\r\n- Mihawk's newest PS5 Mesa/RADV driver. See [the driver's notes](https://github.com/mihawk-99/PS5_Mesa).\r\n- `prod.keys` is read once, not\r\n  at every start.\r\n\r\n> [!WARNING]\r\n> Back up your saves first.\r\n\r\n### Thank you\r\n\r\nThanks to everyone testing on Discord — and to the **Eden developers**.",
  "reactions": {
    "url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/416227100/reactions",
    "total_count": 2,
    "+1": 1,
    "-1": 0,
    "laugh": 0,
    "hooray": 0,
    "confused": 0,
    "heart": 1,
    "rocket": 0,
    "eyes": 0
  }
})json";

constexpr const char* kRepository = "Oliv3rmoon/Nightshade";
constexpr const char* kApi = "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/latest";
constexpr const char* kSums = "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.005.100/SHA256SUMS";
constexpr const char* kZip =
    "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.005.100/Nightshade-v1.005.100.zip";
// The repository's earlier name, which GitHub sends on to the new one (api.github.com and github.com).
constexpr const char* kOldRepository = "Oliv3rmoon/ProsperoEden";
constexpr const char* kOldApi = "https://api.github.com/repos/Oliv3rmoon/ProsperoEden/releases/latest";
constexpr const char* kMoved = "https://api.github.com/repositories/1421452418/releases/latest";
constexpr const char* kDigest = "9a726c3a4238bfc9651a8c2bd3952cf07972495a3f930cc6d0a5b487140a5991";
const std::string kSumsText = std::string(kDigest) + "  Nightshade-v1.005.100.zip\n";

// What the What's new view gets from the release's Markdown.
constexpr const char* kNotes =
    "Nightshade v1.005.100. Oliv3rmoon's ProsperoEden, with fixes that aren't in an official release yet.\n"
    "\n"
    "New in 1.005.100\n"
    "\n"
    "Graphics driver\n"
    "- Mihawk's newest PS5 Mesa/RADV driver. See the driver's notes.\n"
    "- prod.keys is read once, not at every start.\n"
    "\n"
    "Warning: Back up your saves first.\n"
    "\n"
    "Thank you\n"
    "\n"
    "Thanks to everyone testing on Discord — and to the Eden developers.";

std::string With(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    assert(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

std::string WithAll(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// The answer without the spaces and line ends between its values.
std::string Compact(const std::string& json) {
    std::string out;
    bool in_string = false, escaped = false;
    for (const char c : json) {
        if (in_string) {
            out += c;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        in_string = c == '"';
        out += c;
    }
    return out;
}

github_release_reason Evaluate(const std::string& json, const char* installed, github_release* release = nullptr) {
    static github_release scratch;
    return github_release_evaluate(json.data(), json.size(), installed, release ? release : &scratch);
}

std::string Notes(const std::string& markdown, std::size_t characters = GITHUB_RELEASE_NOTES_CHARACTERS,
                  int* truncated = nullptr) {
    static char out[16384];
    int cut = 0;
    github_release_notes(markdown.data(), markdown.size(), out, sizeof(out), characters, &cut);
    if (truncated) *truncated = cut;
    return out;
}

int Sums(const std::string& text, std::string* digest = nullptr) {
    char sha256[65] = {};
    const int found = github_release_sums(text.data(), text.size(), "Nightshade-v1.005.100.zip", sha256);
    if (digest) *digest = sha256;
    return found;
}

// The network, stood in for: what each address answers, and every address asked, in order.
struct Network {
    std::map<std::string, std::pair<int, std::string>> answers; // fetch: status and body
    std::map<std::string, std::string> files;                   // download: the file (status 200)
    std::vector<std::string> asked;
    int failure = 0; // every request fails with this code
};
Network network;

int Fetch(void*, const char* url, char* body, size_t capacity, size_t* length, int* status) {
    network.asked.push_back(url);
    *length = 0;
    *status = 0;
    if (network.failure) return network.failure;
    const auto found = network.answers.find(url);
    if (found == network.answers.end()) {
        *status = 404;
        return 0;
    }
    if (found->second.second.size() > capacity) { // as console_fetch: told from a failed transfer
        *status = found->second.first;
        return SELF_UPDATE_FETCH_TOO_LARGE;
    }
    std::memcpy(body, found->second.second.data(), found->second.second.size());
    *length = found->second.second.size();
    *status = found->second.first;
    return 0;
}

// As console_download: only from github.com, at most `limit` bytes, 0 for a complete 200.
int Download(void*, const char* url, std::uint64_t limit, self_update_sink sink, void* user) {
    network.asked.push_back(url);
    if (network.failure) return network.failure;
    if (!self_update_url_allowed(url, 0)) return -1;
    const auto found = network.files.find(url);
    if (found == network.files.end()) return -404;
    if (found->second.size() > limit) return -10023;
    return sink(user, found->second.data(), found->second.size()) == 1 ? 0 : -10023;
}

int Verify(void*, const unsigned char*, const unsigned char*, const void*, size_t) { return 0; }
int OpenHelper(void*, self_update_channel*) { return 0; }
int LoadSequence(void*, std::uint64_t*) { return 0; }
int SaveSequence(void*, std::uint64_t) { return 0; }
std::uint64_t Now(void*) { return 0; }
const self_update_platform kPlatform = {Fetch, Download, Verify, OpenHelper, LoadSequence, SaveSequence, Now, nullptr};

self_update_check_result Check(const char* repository, const char* installed, self_update_offer* offer,
                               github_release_report* report) {
    network.asked.clear();
    return self_update_check_from(&kPlatform, repository, "Nightshade", "PPSA99008", installed, offer, report);
}

void Serve(int status, const std::string& body) {
    network = Network{};
    network.answers[kApi] = {status, body};
    network.files[kSums] = kSumsText;
}

void TagVersions() {
    // make-dist.py: tag = 'v' + contentVersion.removeprefix('0'); the check turns it back.
    for (const char* version : {"01.005.100", "10.000.000", "00.001.000", "09.999.999", "01.000.040"}) {
        const std::string tag = std::string("v") + (version[0] == '0' ? version + 1 : version);
        char back[12] = {};
        assert(github_release_tag_version(tag.c_str(), back) == 1 && std::strcmp(back, version) == 0);
    }
    for (const char* tag : {"1.005.100", "v01.005.100", "V1.005.100", "v1.5.100", "v1.005.100-rc1", "v1.005.1000",
                            "v1.005.10a", "v100.000.000", "oliv3rmoon-build-3", "v", ""})
        assert(github_release_tag_version(tag, nullptr) == 0);
}

void Addresses() {
    char url[256];
    assert(github_release_url(url, sizeof(url), kRepository) != 0 && std::strcmp(url, kApi) == 0);
    for (const char* bad : {"", "Oliv3rmoon", "Oliv3rmoon/", "/ProsperoEden", "Oliv3rmoon/Nightshade/x", "-x/y",
                            "a b/c", "x/..", "x/.", "x/y?z", "x/y#z", "x/y@z", "x\\y/z",
                            "an-owner-name-that-is-forty-letters-long/x"})
        assert(github_release_url(url, sizeof(url), bad) == 0 && url[0] == '\0');
    assert(github_release_repository_valid("o-1/.github") && github_release_repository_valid("x/y.z_w-1"));
    assert(github_release_url(url, 40, kRepository) == 0);
    assert(github_release_file_url(url, sizeof(url), kRepository, "v1.005.100", "Nightshade-v1.005.100.zip") != 0 &&
           std::strcmp(url, kZip) == 0);
    assert(github_release_file_url(url, sizeof(url), kRepository, "oliv3rmoon-build-3", "SHA256SUMS") == 0);
    assert(github_release_file_url(url, sizeof(url), kRepository, "v1.005.100", "../SHA256SUMS") == 0);

    // A download starts on github.com, and its redirect may go only to GitHub's release file host;
    // https only, no other host, no user name, nothing that isn't printable ASCII.
    assert(self_update_url_allowed(kZip, 0) == 1 && self_update_url_allowed(kSums, 0) == 1);
    const std::string redirect = "https://release-assets.githubusercontent.com/github-production-release-asset/"
                                 "1396833161/f618dd6a?sp=r&sv=2018-11-09&sr=b&spr=https&se=2026-10-25T06%3A41%3A47Z&"
                                 "rscd=attachment%3B+filename%3DSHA256SUMS&sig=c1jZCoE0drhtFeXCCTjdGZE7wBc7h2GgUraO%2F";
    assert(self_update_url_allowed(redirect.c_str(), 1) == 1);
    assert(self_update_url_allowed(redirect.c_str(), 0) == 0); // never the first address
    assert(self_update_url_allowed("http://github.com/Oliv3rmoon/Nightshade/releases/download/v1.005.100/x.zip", 0) == 0);
    assert(self_update_url_allowed("http://release-assets.githubusercontent.com/github-production-release-asset/1", 1) == 0);
    // GitHub's former file host is not on the list: release files go to release-assets now.
    assert(self_update_url_allowed("https://objects.githubusercontent.com/github-production-release-asset-2e65be/1", 1) == 0);
    assert(self_update_url_allowed("https://github.com.example.net/Oliv3rmoon/Nightshade/releases/x.zip", 1) == 0);
    assert(self_update_url_allowed("https://release-assets.githubusercontent.com.example.net/x/y/z", 1) == 0);
    assert(self_update_url_allowed("https://example.net/?https://github.com/Oliv3rmoon/x.zip", 1) == 0);
    assert(self_update_url_allowed("https://user@github.com/Oliv3rmoon/Nightshade/x.zip", 0) == 0);
    assert(self_update_url_allowed("https://github.com/Oliv3rmoon/Nightshade/x y.zip", 0) == 0);
    assert(self_update_url_allowed((redirect + std::string(4096, 'a')).c_str(), 1) == 0);

    // The release's own request follows GitHub's redirect for a renamed repository, to
    // api.github.com and nowhere else.
    assert(github_release_api_url_allowed(kMoved) == 1);
    assert(github_release_api_url_allowed(kApi) == 1);
    for (const char* elsewhere : {"http://api.github.com/repositories/1/releases/latest",
                                  "https://api.github.com.example.net/repositories/1/releases/latest",
                                  "https://example.net/https://api.github.com/repositories/1",
                                  "https://github.com/Oliv3rmoon/ProsperoEden/releases/latest",
                                  "https://uploads.github.com/repos/Oliv3rmoon/Nightshade/releases",
                                  "https://user@api.github.com/repositories/1/releases/latest",
                                  "https://api.github.com/repositories/1/releases/latest#x",
                                  "https://api.github.com/repositories/1 /releases/latest", "https://api.github.com/", ""})
        assert(github_release_api_url_allowed(elsewhere) == 0);
    assert(github_release_api_url_allowed(("https://api.github.com/" + std::string(240, 'a')).c_str()) == 0);
}

void SumsLines() {
    std::string digest;
    assert(Sums(kSumsText, &digest) == 1 && digest == kDigest);
    // Among other files' lines, sha256sum's binary mark, capitals, CRLF, no last line end.
    const std::string other = std::string(64, 'a') + "  ProsperoEden-v1.005.000.zip\n";
    assert(Sums(other + kSumsText + other, &digest) == 1 && digest == kDigest);
    assert(Sums(std::string(kDigest) + " *Nightshade-v1.005.100.zip", &digest) == 1 && digest == kDigest);
    assert(Sums("9A726C3A4238BFC9651A8C2BD3952CF07972495A3F930CC6D0A5B487140A5991  Nightshade-v1.005.100.zip\r\n",
                &digest) == 1 && digest == kDigest);
    // No line for the ZIP: only other files' (another release line's ZIP of the same version too),
    // a file whose name merely ends or starts like it.
    assert(Sums("") == 0 && Sums(other) == 0);
    assert(Sums(std::string(kDigest) + "  ProsperoEden-v1.005.100.zip\n") == 0);
    assert(Sums(std::string(kDigest) + "  Nightshade-v1.005.100.zip.sig\n") == 0);
    assert(Sums(std::string(kDigest) + "  MyNightshade-v1.005.100.zip\n") == 0);
    // Its line malformed (short, not hexadecimal, one space, something before it) or there twice.
    assert(Sums(std::string(kDigest).substr(1) + "  Nightshade-v1.005.100.zip\n") == -1);
    assert(Sums(std::string(63, 'a') + "g  Nightshade-v1.005.100.zip\n") == -1);
    assert(Sums(std::string(kDigest) + " Nightshade-v1.005.100.zip\n") == -1);
    assert(Sums(" " + kSumsText) == -1);
    assert(Sums(kSumsText + kSumsText) == -1);
    // Another file's malformed line is not this one's business.
    assert(Sums("zz  ProsperoEden-v1.005.000.zip\n" + kSumsText) == 1);
}

void ReleaseNotes() {
    int truncated = -1;
    // Headings, emphasis, code marks and links go; a paragraph's lines are joined; a callout
    // starts with its kind, as the catalog gives them.
    github_release release{};
    assert(Evaluate(kRelease, "01.005.000", &release) == GITHUB_RELEASE_NEWER);
    assert(std::string(release.notes) == kNotes && release.notes_truncated == 0);
    assert(Notes("one\ntwo\n\n- a\n  b\n- c\n# Title\ntext") == "one two\n\n- a b\n- c\nTitle\ntext");
    assert(Notes("1. first\n2. second\n| a | b |\n| c | d |") == "1. first\n2. second\n| a | b |\n| c | d |");
    assert(Notes("```\ncode **as is**\n```\nafter") == "code **as is**\nafter");
    assert(Notes("[a](https://x/y) and ![img](z.png), <https://example.net/p> \\*not bold\\*") ==
           "a and img, https://example.net/p *not bold*");
    assert(Notes("> [!NOTE]\n> One\n> two\n\n> plain quote\nafter") == "Note: One two\n\nplain quote\nafter");
    assert(Notes("before\n\n---\n\n<!-- hidden\nstill hidden -->\nafter\n\n\n\nend ##") == "before\n\nafter\n\nend ##");
    assert(Notes("## Heading ##\n**Bold only**") == "Heading\nBold only");
    assert(Notes("") == "" && Notes("\n\n  \n") == "");
    // At most 4,000 characters, ending with a whole line: what is left is on the release's page.
    std::string long_notes;
    for (int i = 0; i < 60; ++i) long_notes += "- " + std::string(97, 'x') + "\n";
    const std::string cut = Notes(long_notes, GITHUB_RELEASE_NOTES_CHARACTERS, &truncated);
    assert(truncated == 1 && cut.size() <= 4000 && cut.size() > 3800 && cut.back() == 'x' &&
           cut.size() % 100 == 99);
    assert(Notes(long_notes.substr(0, 3000), GITHUB_RELEASE_NOTES_CHARACTERS, &truncated).size() == 2999 && truncated == 0);
    // One line longer than that alone is cut between characters, not inside one.
    std::string accents;
    for (int i = 0; i < 5000; ++i) accents += "\xc3\xa9";
    const std::string part = Notes(accents, GITHUB_RELEASE_NOTES_CHARACTERS, &truncated);
    assert(truncated == 1 && part.size() == 8000 && part == accents.substr(0, 8000));
    // The notes of a release go into the offer whole, up to the end of the offer's buffer.
    std::string emoji;
    for (int i = 0; i < 4100; ++i) emoji += "\xf0\x9f\x8e\xae";
    assert(Notes(emoji, GITHUB_RELEASE_NOTES_CHARACTERS, &truncated).size() == 16000 && truncated == 1);
}

void Releases() {
    const std::string compact = Compact(kRelease);
    assert(compact.find("\"tag_name\":\"v1.005.100\"") != std::string::npos && compact.size() < kRelease.size());
    // A newer release with its ZIP and SHA256SUMS: the files' names, sizes and the content version.
    for (const std::string& json : {kRelease, compact, WithAll(kRelease, "\\r\\n", "\\n")}) {
        github_release release{};
        assert(Evaluate(json, "01.005.000", &release) == GITHUB_RELEASE_NEWER);
        assert(std::strcmp(release.tag, "v1.005.100") == 0 && std::strcmp(release.available, "01.005.100") == 0);
        assert(release.zip_count == 1 && std::strcmp(release.zips[0], "Nightshade-v1.005.100.zip") == 0 &&
               release.zip_sizes[0] == 42651073 && release.sums_size == 94 && release.zip[0] == '\0');
        assert(std::string(release.notes) == kNotes);
    }
    // Newer by any of its three numbers; the same version or an older one is nothing to offer.
    assert(Evaluate(kRelease, "01.004.999") == GITHUB_RELEASE_NEWER);
    assert(Evaluate(kRelease, "00.999.999") == GITHUB_RELEASE_NEWER);
    assert(Evaluate(kRelease, "01.005.100") == GITHUB_RELEASE_NOT_NEWER);
    assert(Evaluate(kRelease, "01.005.101") == GITHUB_RELEASE_NOT_NEWER);
    assert(Evaluate(kRelease, "02.000.000") == GITHUB_RELEASE_NOT_NEWER);
    assert(Evaluate(WithAll(kRelease, "v1.005.100", "v10.000.000"), "09.999.999") == GITHUB_RELEASE_NEWER);
    assert(Evaluate(kRelease, "development") == GITHUB_RELEASE_BAD_ARGUMENT);
    // A tag that isn't a content version: the fork's earlier test builds, or anything else.
    assert(Evaluate(With(kRelease, "\"tag_name\": \"v1.005.100\"", "\"tag_name\": \"oliv3rmoon-build-3\""), "01.005.000") ==
           GITHUB_RELEASE_NOT_A_VERSION);
    assert(Evaluate(With(kRelease, "\"tag_name\": \"v1.005.100\"", "\"tag_name\": \"" + std::string(60, 'v') + "\""),
                    "01.005.000") == GITHUB_RELEASE_NOT_A_VERSION);
    assert(Evaluate(With(kRelease, "\"tag_name\": \"v1.005.100\"", "\"tag_name\": 7"), "01.005.000") ==
           GITHUB_RELEASE_BAD_RESPONSE);
    // A pre-release or a draft is never offered (releases/latest gives neither).
    assert(Evaluate(With(kRelease, "\"prerelease\": false", "\"prerelease\": true"), "01.005.000") ==
           GITHUB_RELEASE_NOT_PUBLISHED);
    assert(Evaluate(With(kRelease, "\"draft\": false", "\"draft\": true"), "01.005.000") == GITHUB_RELEASE_NOT_PUBLISHED);
    assert(Evaluate(With(kRelease, "\"draft\": false", "\"draft\": \"no\""), "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    // Without both files, uploaded in full, there is no offer: a ZIP is any name ending in
    // -<tag>.zip, and nothing else.
    for (const char* other : {"Nightshade-v1.005.100-test.zip", "Nightshade-v1.005.100.zip.sig", "-v1.005.100.zip",
                              "Nightshade v1.005.100.zip", "Nightshade-v1.005.000.zip", "Nightshade-1.005.100.zip"})
        assert(Evaluate(With(kRelease, "\"name\": \"Nightshade-v1.005.100.zip\"", std::string("\"name\": \"") + other + "\""),
                        "01.005.000") == GITHUB_RELEASE_NO_ZIP);
    {
        github_release release{};
        assert(Evaluate(With(kRelease, "\"name\": \"Nightshade-v1.005.100.zip\"", "\"name\": \"ProsperoEden-v1.005.100.zip\""),
                        "01.005.000", &release) == GITHUB_RELEASE_NEWER &&
               std::strcmp(release.zips[0], "ProsperoEden-v1.005.100.zip") == 0);
    }
    assert(Evaluate(With(kRelease, "\"state\": \"uploaded\",\n      \"size\": 42651073",
                         "\"state\": \"starter\",\n      \"size\": 42651073"), "01.005.000") == GITHUB_RELEASE_NO_ZIP);
    assert(Evaluate(With(kRelease, "\"size\": 42651073", "\"size\": 3000000000"), "01.005.000") == GITHUB_RELEASE_NO_ZIP);
    assert(Evaluate(With(kRelease, "\"size\": 42651073", "\"size\": 0"), "01.005.000") == GITHUB_RELEASE_NO_ZIP);
    assert(Evaluate(With(kRelease, "\"name\": \"SHA256SUMS\"", "\"name\": \"SHA256SUMS.txt\""), "01.005.000") ==
           GITHUB_RELEASE_NO_SUMS);
    assert(Evaluate(With(kRelease, "\"size\": 94", "\"size\": 5000"), "01.005.000") == GITHUB_RELEASE_NO_SUMS);
    const std::size_t assets = compact.find("\"assets\":[") + 10;
    const std::string no_files = compact.substr(0, assets) + compact.substr(compact.find("],\"tarball_url\""));
    assert(Evaluate(no_files, "01.005.000") == GITHUB_RELEASE_NO_ZIP);
    // Not GitHub's answer: cut short, a page of a network's own, a field twice.
    assert(Evaluate(kRelease.substr(0, kRelease.size() / 2), "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate(compact.substr(0, compact.size() - 1), "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate("<html>blocked</html>", "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate("{\"message\":\"Not Found\",\"status\":\"404\"}", "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate(With(kRelease, "\"draft\": false,", "\"draft\": false, \"draft\": false,"), "01.005.000") ==
           GITHUB_RELEASE_BAD_RESPONSE);
    // No notes (null) is still a release.
    github_release release{};
    assert(Evaluate(With(kRelease, kRelease.substr(kRelease.find("\"body\": \""), kRelease.find("\"reactions\"") -
                                                                                  kRelease.find("\"body\": \"")),
                         "\"body\": null,\n  "),
                    "01.005.000", &release) == GITHUB_RELEASE_NEWER &&
           release.notes[0] == '\0');
}

void ChooseZip() {
    // Two ZIPs of the version: SHA256SUMS says which one is the release.
    const std::string second = R"json({
      "name": "ProsperoEden-v1.005.100.zip",
      "state": "uploaded",
      "size": 1000
    },
    {
      "url": "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/assets/652447818",)json";
    const std::string both = With(kRelease, "{\n      \"url\": \"https://api.github.com/repos/Oliv3rmoon/Nightshade/releases/assets/652447818\",", second);
    github_release release{};
    char sha256[65];
    assert(Evaluate(both, "01.005.000", &release) == GITHUB_RELEASE_NEWER && release.zip_count == 2);
    assert(github_release_choose(&release, kSumsText.data(), kSumsText.size(), sha256) == GITHUB_RELEASE_NEWER);
    assert(std::strcmp(release.zip, "Nightshade-v1.005.100.zip") == 0 && release.zip_size == 42651073 &&
           std::strcmp(sha256, kDigest) == 0);
    const std::string other = std::string(64, 'b') + "  ProsperoEden-v1.005.100.zip\n";
    assert(github_release_choose(&release, other.data(), other.size(), sha256) == GITHUB_RELEASE_NEWER);
    assert(std::strcmp(release.zip, "ProsperoEden-v1.005.100.zip") == 0 && release.zip_size == 1000 &&
           sha256 == std::string(64, 'b'));
    // Both listed: which one is meant can't be said. Neither, or a malformed line: no ZIP.
    const std::string two = kSumsText + other;
    assert(github_release_choose(&release, two.data(), two.size(), sha256) == GITHUB_RELEASE_SUMS_TWO_ZIPS &&
           release.zip[0] == '\0' && sha256[0] == '\0');
    const std::string none = std::string(64, 'c') + "  Nightshade-v1.005.000.zip\n";
    assert(github_release_choose(&release, none.data(), none.size(), sha256) == GITHUB_RELEASE_SUMS_NO_LINE);
    const std::string bad = "xyz  ProsperoEden-v1.005.100.zip\n" + kSumsText;
    assert(github_release_choose(&release, bad.data(), bad.size(), sha256) == GITHUB_RELEASE_SUMS_MALFORMED);
}

void GitHubCheck() {
    static self_update_offer offer;
    github_release_report report{};
    // Offered: the ZIP's address made from the repository and the tag, the SHA-256 from SHA256SUMS,
    // the size from the release, its notes; two requests, the release and SHA256SUMS.
    Serve(200, Compact(kRelease));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(report.reason == GITHUB_RELEASE_NEWER && report.http_status == 200 && std::strcmp(report.tag, "v1.005.100") == 0 &&
           std::strcmp(report.zip, "Nightshade-v1.005.100.zip") == 0 && report.redirects == 0);
    assert((network.asked == std::vector<std::string>{kApi, kSums}));
    assert(std::strcmp(offer.title, "PPSA99008") == 0 && std::strcmp(offer.name, "Nightshade") == 0);
    assert(std::strcmp(offer.installed, "01.005.000") == 0 && std::strcmp(offer.available, "01.005.100") == 0);
    assert(std::strcmp(offer.version, "v1.005.100") == 0 && std::strcmp(offer.artifact, kZip) == 0);
    assert(std::strcmp(offer.sha256, kDigest) == 0 && offer.size == 42651073);
    assert(std::strcmp(offer.page, "https://github.com/Oliv3rmoon/Nightshade/releases/tag/v1.005.100") == 0);
    assert(std::string(offer.notes) == kNotes && offer.notes_truncated == 0);
    // What the download then takes: an address it may start at.
    assert(self_update_url_allowed(offer.artifact, 0) == 1);

    // Nothing to say, never an error: no release (or only pre-releases), the rate limit, another
    // status, no network.
    for (const auto& [status, reason] : std::vector<std::pair<int, github_release_reason>>{
             {404, GITHUB_RELEASE_NONE}, {403, GITHUB_RELEASE_RATE_LIMITED}, {429, GITHUB_RELEASE_RATE_LIMITED},
             {500, GITHUB_RELEASE_HTTP_STATUS}, {304, GITHUB_RELEASE_HTTP_STATUS}}) {
        Serve(status, "{\"message\":\"Not Found\"}");
        assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN);
        assert(report.reason == reason && report.http_status == status && offer.artifact[0] == '\0');
        assert((network.asked == std::vector<std::string>{kApi}));
    }
    Serve(200, kRelease);
    network.failure = -10006;
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN);
    assert(report.reason == GITHUB_RELEASE_NETWORK && report.platform_error == -10006);

    // The same version, an older one, or a tag that isn't one: SHA256SUMS is not asked for.
    Serve(200, kRelease);
    assert(Check(kRepository, "01.005.100", &offer, &report) == SELF_UPDATE_UP_TO_DATE &&
           report.reason == GITHUB_RELEASE_NOT_NEWER && offer.available[0] == '\0');
    assert((network.asked == std::vector<std::string>{kApi}));
    assert(Check(kRepository, "01.006.000", &offer, &report) == SELF_UPDATE_UP_TO_DATE);
    Serve(200, WithAll(kRelease, "v1.005.100", "oliv3rmoon-build-4"));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_NOT_A_VERSION && std::strcmp(report.tag, "oliv3rmoon-build-4") == 0);
    assert((network.asked == std::vector<std::string>{kApi}));

    // SHA256SUMS missing from the file host, without the ZIP's line, or with a malformed one.
    Serve(200, kRelease);
    network.files.clear();
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_UNREADABLE && offer.artifact[0] == '\0');
    network.files[kSums] = std::string(64, 'a') + "  ProsperoEden-v1.005.000.zip\n";
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_NO_LINE);
    network.files[kSums] = "not-a-digest  Nightshade-v1.005.100.zip\n";
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_MALFORMED && offer.sha256[0] == '\0');
    network.files[kSums] = std::string(5000, 'a');
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_UNREADABLE);

    // A renamed repository: a build that still names the old one gets GitHub's 301 (or 307), whose
    // body names the new address on api.github.com, as its Location header does; that is asked
    // instead. The files' addresses keep the old name, which github.com sends on to the new one.
    // A redirect anywhere else, without an address, or one after another is not followed.
    const std::string old_sums = "https://github.com/Oliv3rmoon/ProsperoEden/releases/download/v1.005.100/SHA256SUMS";
    const auto redirect = [](int status, const std::string& to) {
        return std::make_pair(status, "{\"message\":\"Moved Permanently\",\"url\":\"" + to +
                                          "\",\"documentation_url\":\"https://docs.github.com/rest/guides/"
                                          "best-practices-for-using-the-rest-api#follow-redirects\"}");
    };
    const auto moved = [&](int status) {
        Serve(200, Compact(kRelease));
        network.answers.clear();
        network.answers[kOldApi] = redirect(status, kMoved);
        network.answers[kMoved] = {200, Compact(kRelease)};
        network.files[old_sums] = kSumsText;
    };
    for (const int status : {301, 307, 302, 308}) {
        moved(status);
        assert(Check(kOldRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE);
        assert(report.redirects == 1 && report.http_status == 200 && std::strcmp(offer.sha256, kDigest) == 0);
        assert(std::strcmp(offer.artifact,
                           "https://github.com/Oliv3rmoon/ProsperoEden/releases/download/v1.005.100/"
                           "Nightshade-v1.005.100.zip") == 0 &&
               self_update_url_allowed(offer.artifact, 0) == 1);
        assert((network.asked == std::vector<std::string>{kOldApi, kMoved, old_sums}));
    }
    moved(301);
    network.answers[kMoved] = {404, "{\"message\":\"Not Found\"}"}; // only pre-releases there yet
    assert(Check(kOldRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_NONE && report.redirects == 1 && report.http_status == 404);
    for (const std::string& to : {std::string("https://example.net/repositories/1421452418/releases/latest"),
                                  std::string("http://api.github.com/repositories/1421452418/releases/latest"),
                                  std::string("https://github.com/Oliv3rmoon/Nightshade/releases/latest")}) {
        moved(301);
        network.answers[kOldApi] = redirect(301, to);
        network.answers[to] = {200, kRelease};
        assert(Check(kOldRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
               report.reason == GITHUB_RELEASE_REDIRECT_REFUSED && report.http_status == 301);
        assert((network.asked == std::vector<std::string>{kOldApi}));
    }
    Serve(301, "{\"message\":\"Moved Permanently\"}");
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_REDIRECT_REFUSED);
    Serve(200, kRelease);
    network.answers[kApi] = redirect(301, kApi); // to itself, again and again
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_REDIRECT_REFUSED && network.asked.size() == 1 + GITHUB_RELEASE_REDIRECTS);

    // A repository, title ID or installed version that can't be used asks nothing.
    Serve(200, kRelease);
    assert(Check("Oliv3rmoon", "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_BAD_ARGUMENT && network.asked.empty());
    assert(Check(kRepository, "development", &offer, &report) == SELF_UPDATE_UNKNOWN && network.asked.empty());
    assert(self_update_check_from(&kPlatform, kRepository, "ProsperoEden", "../PPSA99008", "01.005.000", &offer,
                                  &report) == SELF_UPDATE_UNKNOWN);
}

// The release's notes in the answer, in place of the fixture's.
std::string WithBody(const std::string& body) {
    const std::size_t from = kRelease.find("\"body\": \""), to = kRelease.find("\"reactions\"");
    return kRelease.substr(0, from) + "\"body\": \"" + body + "\",\n  " + kRelease.substr(to);
}

void AnswerSize() {
    static self_update_offer offer;
    github_release_report report{};
    // As long as GitHub lets notes be (125,000 characters), in Chinese: some 380 KB of answer,
    // offered, with its notes cut to what the dialog takes.
    std::string notes;
    for (int i = 0; i < 125000; ++i) notes += "\xe6\xbc\xa2"; // U+6F22
    Serve(200, WithBody(notes));
    assert(network.answers[kApi].second.size() > 375000);
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE && report.reason == GITHUB_RELEASE_NEWER);
    assert(offer.notes_truncated == 1 && std::string(offer.notes) == notes.substr(0, 3 * GITHUB_RELEASE_NOTES_CHARACTERS));
    // Up to 1 MiB in all; a longer answer is too large, said as such (not as a network failure).
    Serve(200, WithBody(std::string(GITHUB_RELEASE_MAX_ANSWER - 8000, 'x')));
    assert(network.answers[kApi].second.size() <= GITHUB_RELEASE_MAX_ANSWER);
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE);
    Serve(200, WithBody(std::string(GITHUB_RELEASE_MAX_ANSWER, 'x')));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN);
    assert(report.reason == GITHUB_RELEASE_TOO_LARGE && report.http_status == 200 && report.platform_error == 0 &&
           std::strcmp(github_release_reason_text(report.reason), "too-large") == 0);
    assert((network.asked == std::vector<std::string>{kApi}) && offer.artifact[0] == '\0');
}

// The release with these files in place of its own.
std::string WithAssets(const std::string& assets) {
    const std::string compact = Compact(kRelease);
    const std::size_t from = compact.find("\"assets\":[") + 10, to = compact.find("],\"tarball_url\"");
    return compact.substr(0, from) + assets + compact.substr(to);
}

std::string Asset(const std::string& name, unsigned size = 10, const char* state = "uploaded") {
    return "{\"name\":\"" + name + "\",\"state\":\"" + state + "\",\"size\":" + std::to_string(size) + "}";
}

void AssetList() {
    // The list is read once, element after element.
    const std::string array = "[ 1, \"a\" ,{\"b\":[2,{}]} , null ]";
    std::size_t at = 0;
    const char* value = nullptr;
    std::size_t length = 0;
    std::vector<std::string> seen;
    while (update_check_json_array_next(array.data(), array.size(), &at, &value, &length) == 1)
        seen.emplace_back(value, length);
    assert((seen == std::vector<std::string>{"1", "\"a\"", "{\"b\":[2,{}]}", "null"}));
    assert(update_check_json_array_next(array.data(), array.size(), &at, &value, &length) == 0);
    at = 0;
    assert(update_check_json_array_next("[ ]", 3, &at, &value, &length) == 0 &&
           update_check_json_array_next("[ ]", 3, &at, &value, &length) == 0);
    for (const std::string bad : {"[1,]", "[1 2]", "[1] x", "{\"a\":1}", "[", "", "[\"open]"}) {
        at = 0;
        int found = 1;
        while (found == 1) found = update_check_json_array_next(bad.data(), bad.size(), &at, &value, &length);
        assert(found == -1);
    }

    static self_update_offer offer;
    github_release_report report{};
    const std::string zip = Asset("Nightshade-v1.005.100.zip", 42651073), sums = Asset("SHA256SUMS", 94);
    // All of GitHub's 1,000 files are looked at: the release's ZIP may come last.
    std::string many;
    for (int i = 0; i < 998; ++i) many += Asset("file-" + std::to_string(i) + ".bin") + ",";
    Serve(200, WithAssets(many + sums + "," + zip));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE &&
           std::strcmp(offer.artifact, kZip) == 0 && offer.size == 42651073);
    // Up to eight ZIPs of the version, SHA256SUMS choosing; a ninth is not dropped (eight decoys
    // before the release's would leave it out) but refused, saying so.
    std::string decoys;
    for (int i = 0; i < 7; ++i) decoys += Asset("decoy" + std::to_string(i) + "-v1.005.100.zip") + ",";
    Serve(200, WithAssets(decoys + zip + "," + sums));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_AVAILABLE && std::strcmp(offer.artifact, kZip) == 0);
    decoys += Asset("decoy7-v1.005.100.zip") + ",";
    Serve(200, WithAssets(decoys + zip + "," + sums));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN);
    assert(report.reason == GITHUB_RELEASE_TOO_MANY_ZIPS && std::strcmp(github_release_reason_text(report.reason), "too-many-zips") == 0);
    assert((network.asked == std::vector<std::string>{kApi}));
    // ZIPs that can't be the release (not uploaded in full, empty) are not counted.
    Serve(200, WithAssets(decoys + Asset("decoy8-v1.005.100.zip", 10, "starter") + "," + Asset("decoy9-v1.005.100.zip", 0) +
                          "," + sums));
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_NO_LINE);
}

void CatalogUnchanged() {
    static self_update_offer offer, direct;
    github_release_report report{};
    // No repository (empty or none): self_update_check(), the catalog, the same requests in the
    // same order: the catalog's manifest at homebrew.page, then at its mirror. Nothing of GitHub's.
    for (const bool signed_manifest : {false, true}) {
        network = Network{};
        if (signed_manifest) { // a manifest and a signature that doesn't verify: untrusted
            network.answers["https://homebrew.page/api/v1/manifest.json"] = {200, "{\"sequence\":1}"};
            network.answers["https://homebrew.page/api/v1/manifest.sig"] = {200, std::string(64, 's')};
        }
        network.asked.clear();
        const self_update_check_result expected = self_update_check(&kPlatform, "PPSA99008", "01.005.000", &direct);
        const std::vector<std::string> catalog = network.asked;
        assert(!catalog.empty() && catalog[0] == "https://homebrew.page/api/v1/manifest.json");
        assert(expected == (signed_manifest ? SELF_UPDATE_UNTRUSTED : SELF_UPDATE_UNKNOWN));
        for (const char* none : {"", static_cast<const char*>(nullptr)}) {
            report.reason = GITHUB_RELEASE_NO_ZIP;
            assert(Check(none, "01.005.000", &offer, &report) == expected);
            assert(network.asked == catalog && report.reason == GITHUB_RELEASE_NEWER && report.http_status == 0);
            for (const std::string& url : network.asked) assert(url.find("github.com") == std::string::npos);
        }
    }
    // With a repository, the catalog is not asked.
    Serve(404, "{}");
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN);
    for (const std::string& url : network.asked) assert(url.find("homebrew.page") == std::string::npos);
}
} // namespace

int main() {
    // The app's content version (sce_sys/param.json) is NN.NNN.NNN; a release's name is not one.
    unsigned parts[3];
    assert(update_check_version_parse("01.000.040", parts) == 1 && parts[0] == 1 && parts[1] == 0 && parts[2] == 40);
    assert(update_check_version_parse("v1.000.040", parts) == 0);

    // The request names the title and nothing else.
    char url[96];
    assert(update_check_url(url, sizeof(url), "PPSA99008") != 0);
    assert(std::strcmp(url, "https://homebrew.page/api/v1/apps/PPSA99008.json") == 0);
    assert(update_check_url(url, sizeof(url), "../PPSA99008") == 0);

    // A newer release is listed: its name is what the notification shows.
    update_check_result result = Decide(Answer("available", "01.000.050", "v1.000.050"), "01.000.040");
    assert(result.state == UPDATE_CHECK_AVAILABLE && std::strcmp(result.version, "v1.000.050") == 0);
    assert(std::strcmp(result.available, "01.000.050") == 0 && std::strcmp(result.installed, "01.000.040") == 0);
    // The numbers compare as numbers, field by field.
    assert(Decide(Answer("available", "01.001.000", "v1.001.000"), "01.000.990").state == UPDATE_CHECK_AVAILABLE);
    assert(Decide(Answer("available", "02.000.000", "v2.000.000"), "01.999.999").state == UPDATE_CHECK_AVAILABLE);

    // The same release, or an older one than this build: nothing to say.
    assert(Decide(Answer("available", "01.000.040", "v1.000.040"), "01.000.040").state == UPDATE_CHECK_UP_TO_DATE);
    assert(Decide(Answer("available", "01.000.040", "v1.000.040"), "01.000.050").state == UPDATE_CHECK_UP_TO_DATE);

    // Anything else is silence: the catalog knows no content version (a release from before the
    // app carried one), the app is not released there, or the answer is not the documented one.
    result = Decide(Answer("available", nullptr, "v1.000.040"), "01.000.040");
    assert(result.state == UPDATE_CHECK_UNKNOWN && result.reason == UPDATE_CHECK_NO_CATALOG_VERSION);
    result = Decide(Answer("coming_soon", "01.000.050", "v1.000.050"), "01.000.040");
    assert(result.state == UPDATE_CHECK_UNKNOWN && result.reason == UPDATE_CHECK_NOT_AVAILABLE);
    result = Decide(Answer("available", "1.0.50", "v1.000.050"), "01.000.040");
    assert(result.state == UPDATE_CHECK_UNKNOWN && result.reason == UPDATE_CHECK_BAD_RESPONSE);
    result = Decide("<html>not found</html>", "01.000.040");
    assert(result.state == UPDATE_CHECK_UNKNOWN && result.reason == UPDATE_CHECK_BAD_RESPONSE);
    result = Decide(Answer("available", "01.000.050", "v1.000.050"), "development");
    assert(result.state == UPDATE_CHECK_UNKNOWN && result.reason == UPDATE_CHECK_BAD_INSTALLED_VERSION);

    TagVersions();
    Addresses();
    SumsLines();
    ReleaseNotes();
    Releases();
    ChooseZip();
    GitHubCheck();
    AnswerSize();
    AssetList();
    CatalogUnchanged();
    std::puts("update check: versions, request address, newer/same/older and silent answers PASS");
    std::puts("update check from GitHub: tag versions, addresses and redirects, SHA256SUMS lines, release notes, "
              "releases/latest answers, the ZIP SHA256SUMS lists, the check, its redirects and silent answers, no "
              "repository is the catalog PASS");
    return 0;
}
