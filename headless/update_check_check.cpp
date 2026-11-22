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

    // The release's own request follows GitHub's redirect for a renamed repository to the address
    // GitHub gives it, exactly, and to nothing else: not another repository's path, another host,
    // port, case or spelling of the same, nothing more after it.
    assert(github_release_api_url_allowed(kMoved) == 1);
    assert(github_release_api_url_allowed("https://api.github.com/repositories/1/releases/latest") == 1);
    assert(github_release_api_url_allowed("https://api.github.com/repositories/9223372036854775807/releases/latest") == 1);
    for (const char* elsewhere : {
             kApi, "https://api.github.com/repos/someone/else/releases/latest",
             "https://api.github.com/repositories/1421452418/releases/1",
             "https://api.github.com/repositories/1421452418/releases",
             "https://api.github.com/repositories/1421452418/tags/latest",
             "https://api.github.com/repositories/1421452418/../1/releases/latest",
             "https://api.github.com/repositories//releases/latest",
             "https://api.github.com/repositories/01421452418/releases/latest",
             "https://api.github.com/repositories/12345678901234567890/releases/latest",
             "https://api.github.com:443/repositories/1421452418/releases/latest",
             "https://API.GITHUB.COM/repositories/1421452418/releases/latest",
             "HTTPS://api.github.com/repositories/1421452418/releases/latest",
             "https://api.github.com/Repositories/1421452418/releases/latest",
             "https://api.github.com/repositories/1421452418/releases/LATEST",
             "https://api.github.com/repositories\\1421452418/releases/latest",
             "https://api.github.com/repositories/1421452418\\releases/latest",
             "https://api.github.com/repositories/1421452418%40example.net/releases/latest",
             "https://api.github.com%40example.net/repositories/1421452418/releases/latest",
             "https://api.github.com/repositories/%31421452418/releases/latest",
             "https://api.github.com./repositories/1421452418/releases/latest",
             "https://api.github.com.example.net/repositories/1421452418/releases/latest",
             "https://user@api.github.com/repositories/1421452418/releases/latest",
             "http://api.github.com/repositories/1421452418/releases/latest",
             "https://api.github.com/repositories/1421452418/releases/latest/",
             "https://api.github.com/repositories/1421452418/releases/latest?per_page=1",
             "https://api.github.com/repositories/1421452418/releases/latest#x",
             "https://api.github.com/repositories/1421452418/releases/latest ",
             " https://api.github.com/repositories/1421452418/releases/latest",
             "https://api.github.com/repositories/1421452418/releases/latest\n", "https://api.github.com/", ""})
        assert(github_release_api_url_allowed(elsewhere) == 0);
    assert(github_release_api_url_allowed(nullptr) == 0);
}

// The address in the redirect as GitHub writes it, in JSON: what counts is what it reads as.
void RedirectTargets() {
    static self_update_offer offer;
    github_release_report report{};
    const std::string old_sums = "https://github.com/Oliv3rmoon/ProsperoEden/releases/download/v1.005.100/SHA256SUMS";
    const auto moved_to = [&](const std::string& json_url) {
        Serve(200, kRelease);
        network.answers.clear();
        network.answers[kOldApi] = {301, "{\"message\":\"Moved Permanently\",\"url\":\"" + json_url + "\"}"};
        network.answers[kMoved] = {200, kRelease};
        network.files[old_sums] = kSumsText;
        return Check(kOldRepository, "01.005.000", &offer, &report);
    };
    // Escapes that read as the very address are followed to it.
    for (const char* same : {"https:\\/\\/api.github.com\\/repositories\\/1421452418\\/releases\\/latest",
                             "\\u0068ttps://api.github.com/repositories/1421452418/releases/latest"}) {
        assert(moved_to(same) == SELF_UPDATE_AVAILABLE && report.redirects == 1);
        assert((network.asked == std::vector<std::string>{kOldApi, kMoved, old_sums}));
    }
    // Ones that read as something else, or not as text, are not.
    for (const char* other : {"https://api.github.com/repositories/1421452418/releases/latest\\u0000",
                              "https://api.github.com/repositories/1421452418/releases/latest\\n",
                              "https://api.github.com\\u002erepositories/1421452418/releases/latest",
                              "https://api.github.com/repositories/1421452418\\u002f..\\u002f1/releases/latest",
                              "https://api.github.com\\u0040example.net/repositories/1421452418/releases/latest",
                              "https://api.github.com/repositories/1421452418\\\\releases/latest",
                              "https://api.github.com/repositories/\\uff11421452418/releases/latest",
                              "https://api.github.com/repositories/1421452418/releases/latest\\"}) {
        assert(moved_to(other) == SELF_UPDATE_UNKNOWN && report.reason == GITHUB_RELEASE_REDIRECT_REFUSED &&
               report.http_status == 301 && report.redirects == 0);
        assert((network.asked == std::vector<std::string>{kOldApi}));
    }
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
    network.answers[kApi] = redirect(301, kMoved);
    network.answers[kMoved] = redirect(301, kMoved); // to itself, again and again
    assert(Check(kRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_REDIRECT_REFUSED && report.redirects == GITHUB_RELEASE_REDIRECTS &&
           network.asked.size() == 1 + GITHUB_RELEASE_REDIRECTS);

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

// A release's tag goes into the log: whatever it holds, it stays on its line, and says what it is.
void LoggedTag() {
    static self_update_offer offer;
    github_release_report report{};
    const auto tagged = [&](const std::string& json_tag) {
        Serve(200, With(kRelease, "\"tag_name\": \"v1.005.100\"", "\"tag_name\": \"" + json_tag + "\""));
        return Check(kRepository, "01.005.000", &offer, &report);
    };
    const auto one_line = [](const char* text) {
        for (const char* at = text; *at != '\0'; ++at)
            if (*at <= ' ' || *at >= 0x7f) return false;
        return true;
    };
    assert(tagged("v1\\n[ProsperoEden] update check: forged") == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_NOT_A_VERSION);
    assert(std::string(report.tag) == "v1\\x0a[ProsperoEden]\\x20update\\x20check:\\x20forged" && one_line(report.tag));
    assert(tagged("a\\rb\\tc\\\\d\u00e9\\u2028e\\u001b[2J") == SELF_UPDATE_UNKNOWN);
    assert(std::string(report.tag) == "a\\x0db\\x09c\\x5cd\\xc3\\xa9\\xe2\\x80\\xa8e\\x1b[2J" && one_line(report.tag));
    // The longest tag read (39 bytes) is shown whole, escaped.
    std::string ends;
    for (int i = 0; i < 39; ++i) ends += "\\n";
    assert(tagged(ends) == SELF_UPDATE_UNKNOWN && std::strlen(report.tag) == 39 * 4 && one_line(report.tag));
    // And so the log line update_notice.cpp writes is one line.
    char line[640];
    std::snprintf(line, sizeof(line), "GitHub %s releases/latest: reason=%s http=%d redirects=%d error=%d tag=%s zip=%s",
                  kRepository, github_release_reason_text(report.reason), report.http_status, report.redirects,
                  report.platform_error, report.tag[0] ? report.tag : "-", report.zip[0] ? report.zip : "-");
    assert(std::strchr(line, '\n') == nullptr && std::strchr(line, '\r') == nullptr);
    // A tag that is a version is shown as it is.
    assert(tagged("v1.005.100") == SELF_UPDATE_AVAILABLE && std::strcmp(report.tag, "v1.005.100") == 0);
}

// A field that is there twice says two things: the answer is read as neither.
void TwiceSaid() {
    assert(Evaluate(With(kRelease, "\"tag_name\": \"v1.005.100\",", "\"tag_name\": \"v1.005.100\", \"tag_name\": \"v9.000.000\","),
                    "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate(With(kRelease, "\"name\": \"Nightshade-v1.005.100.zip\",",
                         "\"name\": \"Nightshade-v1.005.100.zip\", \"name\": \"Nightshade-v1.005.100.bin\","),
                    "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate(With(kRelease, "\"state\": \"uploaded\",\n      \"size\": 42651073",
                         "\"state\": \"uploaded\", \"state\": \"starter\",\n      \"size\": 42651073"),
                    "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    assert(Evaluate(With(kRelease, "\"body\": \"", "\"body\": \"Other notes\", \"body\": \""), "01.005.000") ==
           GITHUB_RELEASE_BAD_RESPONSE);
    // Already so: a size, draft, prerelease or assets twice.
    assert(Evaluate(With(kRelease, "\"size\": 42651073", "\"size\": 42651073, \"size\": 1"), "01.005.000") ==
           GITHUB_RELEASE_NO_ZIP);
    assert(Evaluate(With(kRelease, "\"prerelease\": false,", "\"prerelease\": false, \"prerelease\": true,"),
                    "01.005.000") == GITHUB_RELEASE_BAD_RESPONSE);
    // A redirect naming two addresses is not followed, even when the first is GitHub's.
    static self_update_offer offer;
    github_release_report report{};
    Serve(200, kRelease);
    network.answers.clear();
    network.answers[kOldApi] = {301, std::string("{\"message\":\"Moved Permanently\",\"url\":\"") + kMoved +
                                         "\",\"url\":\"https://example.net/releases/latest\"}"};
    network.answers[kMoved] = {200, kRelease};
    assert(Check(kOldRepository, "01.005.000", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_REDIRECT_REFUSED);
    assert((network.asked == std::vector<std::string>{kOldApi}));
    // Names that are the same in nested objects (the release's and its files') are not twice said.
    assert(Evaluate(kRelease, "01.005.000") == GITHUB_RELEASE_NEWER);
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

// ---- Test builds (Settings > Diagnostics > Test builds) -------------------------------------

constexpr const char* kList = "https://api.github.com/repos/Oliv3rmoon/Nightshade/releases?per_page=100";
constexpr const char* kOldList = "https://api.github.com/repos/Oliv3rmoon/ProsperoEden/releases?per_page=100";
constexpr const char* kMovedList = "https://api.github.com/repositories/1421452418/releases?per_page=100";

// A release of the fixture's shape with another tag: its files, addresses and title follow the tag,
// as the release workflow names them (Nightshade-<tag>.zip and SHA256SUMS).
std::string ReleaseOf(const std::string& tag, bool prerelease, const std::string& title = "") {
    std::string json = WithAll(kRelease, "v1.005.100", tag);
    if (prerelease) json = With(json, "\"prerelease\": false", "\"prerelease\": true");
    if (!title.empty()) json = With(json, "\"name\": \"Nightshade " + tag + "\"", "\"name\": \"" + title + "\"");
    return json;
}

// The releases list as GitHub answers it: an array of releases, in GitHub's order, not the versions'.
std::string ListOf(const std::vector<std::string>& releases) {
    std::string json = "[\n";
    for (std::size_t i = 0; i < releases.size(); ++i) json += (i ? ",\n" : "") + releases[i];
    return json + "\n]";
}

std::string SumsUrl(const std::string& tag) {
    return "https://github.com/Oliv3rmoon/Nightshade/releases/download/" + tag + "/SHA256SUMS";
}
std::string ZipUrl(const std::string& tag) {
    return "https://github.com/Oliv3rmoon/Nightshade/releases/download/" + tag + "/Nightshade-" + tag + ".zip";
}
// Each release's SHA256SUMS gives its ZIP a digest of its own, so the one used says whose it is.
std::string DigestOf(const std::string& tag) {
    std::string digest(64, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) digest[i] = "0123456789abcdef"[(tag[i % tag.size()] + i) % 16];
    return digest;
}

// The repository as it is now (Oliv3rmoon/Nightshade, 2026-11-01), in the order GitHub lists it:
// the 1.010 line (PPSA99008) stable and test, the own title ID's line 1.011 and the Switch 2 test
// line 1.012 (both PPSA64448), the fork's first test builds, whose tags are no versions, and a
// draft (which only the repository's owner would see).
std::vector<std::string> Repository() {
    return {ReleaseOf("v1.010.007", false),
            ReleaseOf("v1.010.008", true, "Nightshade v1.010.008 (test release)"),
            ReleaseOf("v1.010.006", false),
            ReleaseOf("v1.012.001", true, "Nightshade v1.012.001 (Switch 2 test release)"),
            ReleaseOf("v1.012.000", true, "Nightshade v1.012.000 (Switch 2 test release)"),
            ReleaseOf("v1.011.002", true, "Nightshade v1.011.002 (test release)"),
            ReleaseOf("v1.011.000", true, "Nightshade v1.011.000 (test release)"),
            ReleaseOf("v1.010.003", false),
            With(WithAll(ReleaseOf("v1.010.008", true), "v1.010.008", "oliv3rmoon-build-3"),
                 "\"name\": \"Nightshade oliv3rmoon-build-3\"", "\"name\": \"ProsperoEden build 3\""),
            With(ReleaseOf("v1.010.099", false), "\"draft\": false", "\"draft\": true")};
}

const char* kListTags[] = {"v1.010.007", "v1.010.008", "v1.010.006", "v1.012.001", "v1.012.000",
                           "v1.011.002", "v1.011.000", "v1.010.003", "v1.010.099"};

void ServeList(const std::string& list, const char* at = kList) {
    network = Network{};
    network.answers[at] = {200, list};
    for (const char* tag : kListTags) network.files[SumsUrl(tag)] = DigestOf(tag) + "  Nightshade-" + tag + ".zip\n";
}

self_update_check_result TestCheck(const char* repository, const char* installed, self_update_offer* offer,
                                   github_release_report* report) {
    network.asked.clear();
    return self_update_check_tests_from(&kPlatform, repository, "Nightshade", "PPSA99008", installed, offer, report);
}

// Which tag github_release_pick chooses for an install of `installed` ("" for none).
std::string Picked(const std::string& list, const char* installed, github_release_reason* reason = nullptr,
                   int* listed = nullptr) {
    const char* chosen = nullptr;
    std::size_t length = 0;
    const github_release_reason found = github_release_pick(list.data(), list.size(), installed, &chosen, &length, listed);
    if (reason) *reason = found;
    if (found != GITHUB_RELEASE_NEWER) {
        assert(chosen == nullptr && length == 0);
        return "";
    }
    char tag[40] = {};
    assert(update_check_json_string(chosen, length, "tag_name", tag, sizeof(tag)) == 1);
    return tag;
}

void Lines() {
    // The line is the first two numbers; the third is the build on it.
    assert(github_release_same_line("01.010.009", "01.010.008") && github_release_same_line("01.010.008", "01.010.008"));
    assert(github_release_same_line("01.010.000", "01.010.999") && github_release_same_line("10.000.001", "10.000.000"));
    // The own title ID's line, the Switch 2 test line, another major version, an older line: never.
    for (const char* other : {"01.011.000", "01.011.002", "01.012.000", "01.012.001", "02.010.008", "00.010.008",
                              "01.009.999", "11.010.008", "01.100.008", "01.001.008"})
        assert(!github_release_same_line(other, "01.010.008") && !github_release_same_line("01.010.008", other));
    // Anything that isn't a content version is on no line.
    for (const char* bad : {"v1.010.009", "1.010.009", "01.010", "01.010.", "01.010.0089", "01.010.00a", "", "development"})
        assert(!github_release_same_line(bad, "01.010.008") && !github_release_same_line("01.010.008", bad));
    assert(!github_release_same_line(nullptr, "01.010.008") && !github_release_same_line("01.010.008", nullptr));

    // The list's address, and the one a renamed repository's list is sent on to, exactly.
    char url[256];
    assert(github_release_list_url(url, sizeof(url), kRepository) != 0 && std::strcmp(url, kList) == 0);
    assert(github_release_list_url(url, sizeof(url), "Oliv3rmoon") == 0 && url[0] == '\0');
    assert(github_release_list_url(url, sizeof(url), "x/y?per_page=1") == 0);
    assert(github_release_list_url(url, 60, kRepository) == 0 && url[0] == '\0');
    assert(github_release_list_url_allowed(kMovedList) == 1);
    for (const char* elsewhere : {kList, kMoved, "https://api.github.com/repositories/1421452418/releases",
                                  "https://api.github.com/repositories/1421452418/releases?per_page=1",
                                  "https://api.github.com/repositories/1421452418/releases?per_page=100&page=2",
                                  "https://api.github.com/repositories/1421452418/releases?per_page=100#x",
                                  "https://api.github.com/repositories/01421452418/releases?per_page=100",
                                  "https://example.net/repositories/1421452418/releases?per_page=100",
                                  "http://api.github.com/repositories/1421452418/releases?per_page=100",
                                  "https://api.github.com/repos/someone/else/releases?per_page=100", ""})
        assert(github_release_list_url_allowed(elsewhere) == 0);
    assert(github_release_list_url_allowed(nullptr) == 0);
    // And releases/latest's redirect still goes to releases/latest alone, never to a list.
    assert(github_release_api_url_allowed(kMovedList) == 0);
}

void Picking() {
    const std::string list = ListOf(Repository());
    github_release_reason reason{};
    int listed = 0;
    // The newest of the 1.010 line newer than the install, a test build or not, wherever it is in the
    // list: 1.010.008 (a pre-release, listed second) for 1.010.007 and every earlier 1.010 build.
    for (const char* installed : {"01.010.007", "01.010.006", "01.010.003", "01.010.000"}) {
        assert(Picked(list, installed, &reason, &listed) == "v1.010.008" && reason == GITHUB_RELEASE_NEWER);
        assert(listed == 10);
    }
    assert(Picked(Compact(list), "01.010.006") == "v1.010.008");
    // 1.010.008 has nothing newer on its line: the 1.011 and 1.012 releases are newer, and never
    // offered to it; nor is the draft of 1.010.099.
    assert(Picked(list, "01.010.008", &reason) == "" && reason == GITHUB_RELEASE_NOT_NEWER);
    assert(Picked(list, "01.010.099", &reason) == "" && reason == GITHUB_RELEASE_NOT_NEWER);
    // Each line keeps to itself: a 1.011 install gets 1.011.002, a 1.012 one 1.012.001, an install
    // of a line the list doesn't have (1.005, 2.010) nothing.
    assert(Picked(list, "01.011.000") == "v1.011.002" && Picked(list, "01.011.001") == "v1.011.002");
    assert(Picked(list, "01.012.000") == "v1.012.001");
    assert(Picked(list, "01.005.105", &reason) == "" && reason == GITHUB_RELEASE_NOT_NEWER);
    assert(Picked(list, "02.010.000", &reason) == "" && reason == GITHUB_RELEASE_NOT_NEWER);
    assert(Picked(list, "development", &reason) == "" && reason == GITHUB_RELEASE_BAD_ARGUMENT);

    // Version order is the numbers', not the list's: the highest third number, wherever it is.
    assert(Picked(ListOf({ReleaseOf("v1.010.009", true), ReleaseOf("v1.010.100", true), ReleaseOf("v1.010.010", false),
                          ReleaseOf("v1.010.099", false)}),
                  "01.010.008") == "v1.010.100");
    assert(Picked(ListOf({ReleaseOf("v1.010.010", false), ReleaseOf("v1.010.009", true)}), "01.010.008") ==
           "v1.010.010");
    assert(Picked(ListOf({ReleaseOf("v1.010.009", true), ReleaseOf("v1.010.010", false)}), "01.010.008") ==
           "v1.010.010");
    // A stable release newer than the test builds is the one offered; a test build newer than the
    // stable one is.
    assert(Picked(ListOf({ReleaseOf("v1.010.009", true), ReleaseOf("v1.010.010", false), ReleaseOf("v1.011.003", false)}),
                  "01.010.008") == "v1.010.010");
    assert(Picked(ListOf({ReleaseOf("v1.010.010", false), ReleaseOf("v1.010.011", true), ReleaseOf("v1.012.002", true)}),
                  "01.010.008") == "v1.010.011");
    // A newer draft is passed over, and the newest published one taken.
    assert(Picked(ListOf({With(ReleaseOf("v1.010.012", true), "\"draft\": false", "\"draft\": true"),
                          ReleaseOf("v1.010.009", true)}),
                  "01.010.008") == "v1.010.009");
    // Two releases of a lower version say nothing about the one chosen.
    assert(Picked(ListOf({ReleaseOf("v1.010.009", false), ReleaseOf("v1.010.009", false), ReleaseOf("v1.010.010", true)}),
                  "01.010.008") == "v1.010.010");

    // Nothing at all: no releases.
    assert(Picked("[]", "01.010.008", &reason, &listed) == "" && reason == GITHUB_RELEASE_NONE && listed == 0);
    assert(Picked(" [ ] ", "01.010.008", &reason) == "" && reason == GITHUB_RELEASE_NONE);
    // Not GitHub's list: cut short (anywhere), not an array, a release that isn't an object, a
    // draft or prerelease that isn't true or false, or missing, a tag twice, two releases of the
    // version that would be chosen. Read as nothing, never as part of an answer.
    for (const std::size_t cut : {list.size() / 3, list.size() / 2, list.size() - 2, list.size() - 1})
        assert(Picked(list.substr(0, cut), "01.010.007", &reason) == "" && reason == GITHUB_RELEASE_BAD_RESPONSE);
    for (const std::string& bad :
         {std::string("<html>blocked</html>"), Repository()[1], std::string("{\"message\":\"Not Found\"}"),
          ListOf({ReleaseOf("v1.010.009", true), "7"}), ListOf({ReleaseOf("v1.010.009", true), "null"}),
          ListOf({ReleaseOf("v1.010.009", true), "[]"}),
          ListOf({With(ReleaseOf("v1.010.009", true), "\"draft\": false", "\"draft\": \"no\"")}),
          ListOf({With(ReleaseOf("v1.010.009", true), "\"prerelease\": true", "\"prerelease\": 1")}),
          ListOf({With(ReleaseOf("v1.010.009", true), "\"draft\": false,", "")}),
          ListOf({With(ReleaseOf("v1.010.009", true), "\"tag_name\": \"v1.010.009\",",
                       "\"tag_name\": \"v1.010.009\", \"tag_name\": \"v1.011.009\",")}),
          ListOf({ReleaseOf("v1.010.009", true), ReleaseOf("v1.010.009", false)}),
          ListOf({ReleaseOf("v1.010.009", true), ReleaseOf("v1.010.008", false), ReleaseOf("v1.010.009", true)}),
          ListOf({ReleaseOf("v1.010.009", true)}) + ","})
        assert(Picked(bad, "01.010.007", &reason) == "" && reason == GITHUB_RELEASE_BAD_RESPONSE);
    // A tag that isn't a version (or is too long to be one) is no release of a line, not an error.
    assert(Picked(ListOf({With(ReleaseOf("v1.010.009", true), "\"tag_name\": \"v1.010.009\"",
                               "\"tag_name\": \"" + std::string(60, 'v') + "\""),
                          ReleaseOf("v1.010.008", true)}),
                  "01.010.007") == "v1.010.008");
    assert(Picked(ListOf({With(ReleaseOf("v1.010.009", true), "\"tag_name\": \"v1.010.009\"", "\"tag_name\": null")}),
                  "01.010.007", &reason) == "" &&
           reason == GITHUB_RELEASE_NOT_NEWER);
}

void Listed() {
    // The release chosen is read as releases/latest's is, and a pre-release is offered: its title kept.
    github_release release{};
    assert(github_release_evaluate_listed(Repository()[1].data(), Repository()[1].size(), "01.010.007", &release) ==
           GITHUB_RELEASE_NEWER);
    assert(release.prerelease == 1 && std::strcmp(release.title, "Nightshade v1.010.008 (test release)") == 0);
    assert(std::strcmp(release.tag, "v1.010.008") == 0 && std::strcmp(release.available, "01.010.008") == 0);
    assert(release.zip_count == 1 && std::strcmp(release.zips[0], "Nightshade-v1.010.008.zip") == 0);
    // A stable one is offered as before, not as a pre-release.
    assert(github_release_evaluate_listed(Repository()[0].data(), Repository()[0].size(), "01.010.006", &release) ==
           GITHUB_RELEASE_NEWER && release.prerelease == 0);
    // Without test builds a pre-release is still never offered, whatever asks.
    assert(Evaluate(Repository()[1], "01.010.007") == GITHUB_RELEASE_NOT_PUBLISHED);
    // A draft never is.
    const std::string draft = With(Repository()[1], "\"draft\": false", "\"draft\": true");
    assert(github_release_evaluate_listed(draft.data(), draft.size(), "01.010.007", &release) ==
           GITHUB_RELEASE_NOT_PUBLISHED);
    // The title as the dialog draws it: control characters as spaces; none (null), one longer than
    // the dialog takes, or one not text is no title; one said twice is no answer.
    const auto title = [&](const std::string& value, github_release_reason expected = GITHUB_RELEASE_NEWER) {
        const std::string json = With(Repository()[1], "\"name\": \"Nightshade v1.010.008 (test release)\"", "\"name\": " + value);
        github_release read{};
        assert(github_release_evaluate_listed(json.data(), json.size(), "01.010.007", &read) == expected);
        return std::string(read.title);
    };
    assert(title("\"Nightshade\\tv1.010.008\\n(test \\u0007release)\"") == "Nightshade v1.010.008 (test  release)");
    assert(title("\"Nightshade v1.010.008 \\u2014 test\"") == "Nightshade v1.010.008 \xe2\x80\x94 test");
    assert(title("null") == "" && title("7") == "" && title("\"" + std::string(200, 'x') + "\"") == "");
    assert(title("\"" + std::string(127, 'x') + "\"") == std::string(127, 'x'));
    title("\"a\", \"name\": \"b\"", GITHUB_RELEASE_BAD_RESPONSE);
}

void TestBuildCheck() {
    static self_update_offer offer;
    github_release_report report{};
    const std::string list = ListOf(Repository());

    // An install of 1.010.007 (the stable release) with test builds: 1.010.008, the test build, its
    // ZIP's address made from the repository and its tag, its SHA-256 from its own SHA256SUMS; two
    // requests, the list and that SHA256SUMS. The helper is told what any update tells it.
    ServeList(list);
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert((network.asked == std::vector<std::string>{kList, SumsUrl("v1.010.008")}));
    assert(report.reason == GITHUB_RELEASE_NEWER && report.http_status == 200 && report.listed == 10 &&
           report.prerelease == 1 && std::strcmp(report.tag, "v1.010.008") == 0 &&
           std::strcmp(report.zip, "Nightshade-v1.010.008.zip") == 0);
    assert(std::strcmp(offer.title, "PPSA99008") == 0 && std::strcmp(offer.name, "Nightshade") == 0);
    assert(std::strcmp(offer.installed, "01.010.007") == 0 && std::strcmp(offer.available, "01.010.008") == 0);
    assert(std::strcmp(offer.version, "v1.010.008") == 0 && offer.artifact == ZipUrl("v1.010.008"));
    assert(offer.sha256 == DigestOf("v1.010.008") && offer.size == 42651073 && offer.beside[0] == '\0');
    assert(std::strcmp(offer.page, "https://github.com/Oliv3rmoon/Nightshade/releases/tag/v1.010.008") == 0);
    assert(offer.prerelease == 1 && std::strcmp(offer.release_title, "Nightshade v1.010.008 (test release)") == 0);
    assert(std::string(offer.notes).find("Nightshade v1.010.008.") == 0);
    assert(self_update_url_allowed(offer.artifact, 0) == 1);

    // Without test builds the same repository offers nothing to 1.010.007: releases/latest is 1.010.007
    // itself, and the list is not asked.
    Serve(200, Repository()[0]);
    network.files = {};
    assert(Check(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UP_TO_DATE &&
           report.reason == GITHUB_RELEASE_NOT_NEWER && offer.artifact[0] == '\0');
    assert((network.asked == std::vector<std::string>{kApi}));

    // A stable release found in the list is the update it always was, not a test build.
    ServeList(ListOf({ReleaseOf("v1.010.008", true, "Nightshade v1.010.008 (test release)"), ReleaseOf("v1.010.009", false)}));
    network.files[SumsUrl("v1.010.009")] = DigestOf("v1.010.009") + "  Nightshade-v1.010.009.zip\n";
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(std::strcmp(offer.version, "v1.010.009") == 0 && offer.sha256 == DigestOf("v1.010.009"));
    assert(offer.prerelease == 0 && offer.release_title[0] == '\0' && report.prerelease == 0);

    // The install's own line only: 1.010.008 is up to date though 1.011.002 and 1.012.001 are newer;
    // 1.011.001 is offered 1.011.002; SHA256SUMS is never asked for a release that isn't offered.
    ServeList(list);
    assert(TestCheck(kRepository, "01.010.008", &offer, &report) == SELF_UPDATE_UP_TO_DATE);
    assert(report.reason == GITHUB_RELEASE_NOT_NEWER && report.listed == 10 && offer.artifact[0] == '\0');
    assert((network.asked == std::vector<std::string>{kList}));
    assert(TestCheck(kRepository, "01.011.001", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(std::strcmp(offer.available, "01.011.002") == 0 && offer.artifact == ZipUrl("v1.011.002"));
    assert((network.asked == std::vector<std::string>{kList, SumsUrl("v1.011.002")}));
    for (const char* installed : {"01.010.000", "01.010.003", "01.010.006", "01.010.007"}) {
        assert(TestCheck(kRepository, installed, &offer, &report) == SELF_UPDATE_AVAILABLE);
        assert(github_release_same_line(offer.available, installed) && std::strcmp(offer.available, "01.010.008") == 0);
    }

    // The test build's SHA256SUMS: missing, without its ZIP's line, or another release's line.
    ServeList(list);
    network.files.erase(SumsUrl("v1.010.008"));
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_UNREADABLE && offer.artifact[0] == '\0' && offer.prerelease == 0);
    network.files[SumsUrl("v1.010.008")] = DigestOf("v1.010.007") + "  Nightshade-v1.010.007.zip\n";
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_SUMS_NO_LINE && offer.sha256[0] == '\0');
    // Its ZIP not uploaded yet (the release workflow still at it): nothing now, not an older build.
    {
        std::vector<std::string> releases = Repository();
        releases[1] = With(releases[1], "\"state\": \"uploaded\",\n      \"size\": 42651073",
                           "\"state\": \"starter\",\n      \"size\": 42651073");
        ServeList(ListOf(releases));
        assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
               report.reason == GITHUB_RELEASE_NO_ZIP && std::strcmp(report.tag, "v1.010.008") == 0);
        assert((network.asked == std::vector<std::string>{kList}));
    }

    // Nothing to say, never an error: no repository's list (404), the rate limit, another status,
    // no network, an answer that isn't the list, one longer than 4 MiB.
    for (const auto& [status, reason] : std::vector<std::pair<int, github_release_reason>>{
             {404, GITHUB_RELEASE_NONE}, {403, GITHUB_RELEASE_RATE_LIMITED}, {429, GITHUB_RELEASE_RATE_LIMITED},
             {500, GITHUB_RELEASE_HTTP_STATUS}}) {
        ServeList("{\"message\":\"Not Found\"}");
        network.answers[kList].first = status;
        assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN);
        assert(report.reason == reason && report.http_status == status && offer.artifact[0] == '\0');
        assert((network.asked == std::vector<std::string>{kList}));
    }
    ServeList("[]");
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_NONE);
    ServeList(list.substr(0, list.size() - 1));
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_BAD_RESPONSE);
    ServeList(list);
    network.failure = -10006;
    assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_NETWORK && report.platform_error == -10006);
    // A page of 100 releases with long notes is within the answer's 4 MiB (more than
    // releases/latest's 1 MiB); one past it is too large, said as such.
    {
        std::string notes;
        for (int i = 0; i < 10000; ++i) notes += "\xe6\xbc\xa2"; // U+6F22
        std::vector<std::string> page;
        for (int i = 0; i < GITHUB_RELEASE_LIST_PAGE - 1; ++i) {
            char tag[32];
            std::snprintf(tag, sizeof(tag), "v1.009.%03d", i);
            page.push_back(With(ReleaseOf(tag, true), "\"body\": \"", "\"body\": \"" + notes));
        }
        page.push_back(Repository()[1]);
        ServeList(ListOf(page));
        assert(network.answers[kList].second.size() > 3 * 1024 * 1024 &&
               network.answers[kList].second.size() <= GITHUB_RELEASE_MAX_LIST_ANSWER);
        assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_AVAILABLE &&
               report.listed == GITHUB_RELEASE_LIST_PAGE && std::strcmp(offer.available, "01.010.008") == 0);
        ServeList(ListOf(page) + std::string(GITHUB_RELEASE_MAX_LIST_ANSWER, ' '));
        assert(TestCheck(kRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
               report.reason == GITHUB_RELEASE_TOO_LARGE && report.http_status == 200);
    }

    // A renamed repository: the list's redirect is followed to the list at its new address, and to
    // nothing else (releases/latest's address is not the list's).
    ServeList(list, kMovedList);
    network.answers[kOldList] = {301, std::string("{\"message\":\"Moved Permanently\",\"url\":\"") + kMovedList + "\"}"};
    for (const char* tag : kListTags)
        network.files["https://github.com/Oliv3rmoon/ProsperoEden/releases/download/" + std::string(tag) + "/SHA256SUMS"] =
            DigestOf(tag) + "  Nightshade-" + tag + ".zip\n";
    assert(TestCheck(kOldRepository, "01.010.007", &offer, &report) == SELF_UPDATE_AVAILABLE && report.redirects == 1);
    assert((network.asked == std::vector<std::string>{
                kOldList, kMovedList,
                "https://github.com/Oliv3rmoon/ProsperoEden/releases/download/v1.010.008/SHA256SUMS"}));
    network.answers[kOldList] = {301, std::string("{\"message\":\"Moved Permanently\",\"url\":\"") + kMoved + "\"}"};
    network.answers[kMoved] = {200, Repository()[1]};
    assert(TestCheck(kOldRepository, "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_REDIRECT_REFUSED);
    assert((network.asked == std::vector<std::string>{kOldList}));

    // A repository, title ID or installed version that can't be used asks nothing.
    ServeList(list);
    assert(TestCheck("Oliv3rmoon", "01.010.007", &offer, &report) == SELF_UPDATE_UNKNOWN &&
           report.reason == GITHUB_RELEASE_BAD_ARGUMENT && network.asked.empty());
    assert(TestCheck(kRepository, "development", &offer, &report) == SELF_UPDATE_UNKNOWN && network.asked.empty());
    assert(self_update_check_tests_from(&kPlatform, kRepository, "Nightshade", "../PPSA99008", "01.010.007", &offer,
                                        &report) == SELF_UPDATE_UNKNOWN &&
           network.asked.empty());

    // A build that names no repository asks the catalog, test builds or not, exactly as without them.
    for (const char* none : {"", static_cast<const char*>(nullptr)}) {
        network = Network{};
        static self_update_offer direct;
        const self_update_check_result expected = self_update_check(&kPlatform, "PPSA99008", "01.010.007", &direct);
        const std::vector<std::string> catalog = network.asked;
        assert(!catalog.empty() && catalog[0] == "https://homebrew.page/api/v1/manifest.json");
        assert(TestCheck(none, "01.010.007", &offer, &report) == expected && network.asked == catalog);
        for (const std::string& url : network.asked) assert(url.find("github.com") == std::string::npos);
    }
}
} // namespace

int main(int argc, char** argv) {
    // tools/check-update-check.py: whether the update check takes a repository's name (1) or not
    // (0), for headless/update_repository.cmake to be checked against.
    if (argc == 3 && std::strcmp(argv[1], "--repository") == 0) {
        std::printf("%d\n", github_release_repository_valid(argv[2]));
        return 0;
    }
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
    RedirectTargets();
    SumsLines();
    ReleaseNotes();
    Releases();
    ChooseZip();
    GitHubCheck();
    AnswerSize();
    AssetList();
    LoggedTag();
    TwiceSaid();
    CatalogUnchanged();
    Lines();
    Picking();
    Listed();
    TestBuildCheck();
    std::puts("update check: versions, request address, newer/same/older and silent answers PASS");
    std::puts("update check from GitHub: tag versions, addresses and redirects, SHA256SUMS lines, release notes, "
              "releases/latest answers, the ZIP SHA256SUMS lists, the check, its redirects and silent answers, no "
              "repository is the catalog PASS");
    std::puts("update check with test builds: version lines, the list's address and redirect, the release chosen "
              "(this line only, highest version, pre-release or not, drafts passed over), the list read whole, the "
              "test build's title, its SHA256SUMS and ZIP, without test builds releases/latest alone PASS");
    return 0;
}
