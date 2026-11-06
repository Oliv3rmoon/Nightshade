// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the update check's decision with this app's version numbers
// (update_check/update_check.h, built without its network side), and of a build that asks a
// GitHub repository's releases instead (update_check/github_release.h), through a stand-in for
// the network: the release's answer, its tag's version, SHA256SUMS, the notes, the decision, the
// addresses a download may be made from, and that a build naming no repository asks the catalog
// as before. Then insider builds (update_check/insider.h): the code, the request, the service's
// answers, the fall back to GitHub's test builds, and a download whose SHA-256 differs refused.
#include "update_check/github_release.h"
#include "update_check/insider.h"
#include "update_check/self_update.h"
#include "update_check/self_update_protocol.h"
#include "update_check/self_update_sha256.h"
#include "update_check/update_check.h"
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
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

// ---- Insider builds (update_check/insider.h) --------------------------------------------------

constexpr const char* kInsider = "https://nightly-insider.oliv3rmoon.workers.dev";
constexpr const char* kInsiderUpdates = "https://nightly-insider.oliv3rmoon.workers.dev/v1/updates";
constexpr const char* kInsiderDigest = "5d41402abc4b2a76b9719d911017c592aae4b6e1b2c3d4e5f60718293a4b5c6d";

// A download address the service signs: its token names the build, the expiry and the member.
std::string InsiderUrl(const std::string& tag) {
    return std::string(kInsider) + "/v1/download/eyJrIjoiYnVpbGRzL25pZ2h0c2hhZGUvIiwidCI6IiIsInYiOiI" + tag +
           "In0.Q2xhdWRmbGFyZS1zaWduZWQ_dG9rZW4-MQ";
}

// POST /v1/updates as the service answers "update" (nightly-insider's docs/API.md).
std::string InsiderUpdate(const std::string& version, const std::string& digest = kInsiderDigest,
                          std::uint64_t size = 81234567) {
    const std::string tag = "v" + version;
    return "{\n  \"status\": \"update\",\n  \"app\": \"nightshade\",\n  \"build\": {\n"
           "    \"version\": \"" + version + "\",\n    \"tag\": \"" + tag + "\",\n"
           "    \"title\": \"Nightshade " + tag + " (insider build)\",\n"
           "    \"file\": \"Nightshade-" + tag + ".zip\",\n    \"size\": " + std::to_string(size) + ",\n"
           "    \"sha256\": \"" + digest + "\",\n"
           "    \"notes\": \"Insider build.\\n- The launcher's new settings.\",\n"
           "    \"published_at\": \"2026-11-08T05:00:00Z\",\n    \"public_after\": \"2026-11-15T05:00:00Z\",\n"
           "    \"url\": \"" + InsiderUrl(tag) + "\",\n    \"url_expires_at\": \"2026-11-08T06:00:00Z\"\n  }\n}";
}

std::string InsiderNothing(const char* status) {
    return std::string("{\"status\":\"") + status +
           "\",\"hint\":\"Insider builds: get a code in the nightly Discord, discord.gg/pSqgRU3YP9\"}";
}

insider_status Insider(const std::string& json, int http = 200, const char* installed = "01.010.012",
                       const char* running = nullptr, insider_report* report = nullptr) {
    static self_update_offer offer;
    static insider_report scratch;
    const insider_status status = insider_evaluate(json.data(), json.size(), http, kInsider, "nightshade", "Nightshade",
                                                   "PPSA99008", installed, running, &offer, report ? report : &scratch);
    if (status != INSIDER_UPDATE) assert(offer.artifact[0] == '\0' && offer.sha256[0] == '\0' && offer.size == 0);
    return status;
}

const char* Refused(const std::string& json, const char* installed = "01.010.012", const char* running = nullptr) {
    static insider_report report;
    assert(Insider(json, 200, installed, running, &report) == INSIDER_REFUSED && report.why != nullptr);
    return report.why;
}

// The service, stood in for: what it answers, and every request it was sent.
struct InsiderService {
    struct Request {
        std::string url, agent, json;
    };
    std::vector<Request> asked;
    int status = 200;
    std::string body;
    int result = 0; // the POST's own result: 0, a failure (negative) or SELF_UPDATE_FETCH_TOO_LARGE
};
InsiderService service;

int Post(void* user, const char* url, const char* agent, const char* json, char* body, size_t capacity, size_t* length,
         int* status) {
    assert(user == &service);
    service.asked.push_back({url, agent, json});
    *length = 0;
    *status = 0;
    if (service.result != 0) {
        if (service.result == SELF_UPDATE_FETCH_TOO_LARGE) *status = service.status;
        return service.result;
    }
    if (service.body.size() > capacity) {
        *status = service.status;
        return SELF_UPDATE_FETCH_TOO_LARGE;
    }
    std::memcpy(body, service.body.data(), service.body.size());
    *length = service.body.size();
    *status = service.status;
    return 0;
}

void Answer(int status, const std::string& body) {
    service = InsiderService{};
    service.status = status;
    service.body = body;
}

void InsiderCodes() {
    char code[INSIDER_CODE_LENGTH + 1];
    // As the service takes them: any case, spaces and hyphens, the code as the page shows it.
    for (const char* typed : {"ABCDEFGH", "abcdefgh", "ABCD-EFGH", "abcd efgh", " ab-cd ef-gh ", "aBcD EfGh", "--ABCDEFGH--"}) {
        assert(insider_code_normalise(typed, code) == 1 && std::strcmp(code, "ABCDEFGH") == 0);
    }
    assert(insider_code_normalise("2345-6789", code) == 1 && std::strcmp(code, "23456789") == 0);
    assert(insider_code_normalise("wxyz-wxyz", code) == 1 && std::strcmp(code, "WXYZWXYZ") == 0);
    // Anything else is refused, never repaired: a character outside the alphabet (0 O 1 I L U too,
    // so "ABCD0EFGH" is not "ABCDEFGH"), another separator, another length.
    for (const char* typed : {"", "ABCDEFG", "ABCDEFGHJ", "ABCD0EFGH", "ABCD-EFG0", "ABCD-EFGO", "ABCD-EFG1", "ABCD-EFGI",
                              "ABCD-EFGL", "ABCD-EFGU", "ABCD-EFGH-J", "\xc3\x84" "BCDEFGH", "ABCDEFG\xc3\x84H", "--------",
                              "OIL0U1OI", "ABCD_EFGH", "ABCD.EFGH", "ABCD\tEFGH", "ABCDEFGH\n", "\nABCDEFGH", "ABCD/EFGH"}) {
        code[0] = 'x';
        assert(insider_code_normalise(typed, code) == 0 && code[0] == '\0');
    }
    assert(insider_code_normalise(nullptr, code) == 0 && code[0] == '\0');
    assert(insider_code_normalise("ABCDEFGH", nullptr) == 0);

    // A code as it is sent and kept: 8 characters of the alphabet, nothing else.
    assert(insider_code_valid("ABCDEFGH") == 1 && insider_code_valid("23456789") == 1);
    for (const char* bad : {"abcdefgh", "ABCDEFG", "ABCDEFGHJ", "ABCDEFG0", "ABCD-EFG", "ABCDEFGH\n", ""})
        assert(insider_code_valid(bad) == 0);
    assert(insider_code_valid(nullptr) == 0);
    // The file in the config folder: the code and a line end.
    assert(insider_code_read("ABCDEFGH\n", 9, code) == 1 && std::strcmp(code, "ABCDEFGH") == 0);
    for (const std::string bad : {"ABCDEFGH", "ABCDEFGH\r\n", "abcdefgh\n", "ABCDEFG0\n", "ABCDEFGH\nJ", "ABCDEFGH\n\n",
                                  " ABCDEFGH\n", "", "\n"}) {
        code[0] = 'x';
        assert(insider_code_read(bad.data(), bad.size(), code) == 0 && code[0] == '\0');
    }
    assert(insider_code_read(std::string("ABCD\0FGH\n", 9).data(), 9, code) == 0);

    // The settings say where to get a code after these answers, not after silence or an offer.
    for (const insider_status hints : {INSIDER_NOT_QUALIFIED, INSIDER_INVALID_CODE, INSIDER_EXPIRED_CODE})
        assert(insider_status_hints(hints) == 1);
    for (const insider_status quiet : {INSIDER_UPDATE, INSIDER_NONE, INSIDER_OTHER, INSIDER_NETWORK, INSIDER_TOO_LARGE,
                                       INSIDER_BAD_RESPONSE, INSIDER_REFUSED, INSIDER_BAD_ARGUMENT, INSIDER_NOT_ASKED})
        assert(insider_status_hints(quiet) == 0);
    assert(std::strcmp(insider_status_text(INSIDER_EXPIRED_CODE), "expired_code") == 0 &&
           std::strcmp(insider_status_text(INSIDER_NOT_ASKED), "not-asked") == 0 &&
           std::strcmp(insider_status_text(static_cast<insider_status>(99)), "?") == 0);
}

void InsiderAddresses() {
    // The service's address as a build names it (headless/insider_url.cmake takes the same).
    for (const char* base : {kInsider, "https://insider.example.com/nightly", "https://a.b/c/d", "https://localhost",
                             "https://x-1.y/v_2/a.b-c", "https://x/...", "https://NIGHTLY.example"})
        assert(insider_base_valid(base) == 1);
    for (const char* base : {"", "http://x", "https://", "https://x/", "https://x/a/", "https://-x", "https://x-",
                             "https://x..y", "https://.x", "https://x.", "https://x:8443", "https://user@x",
                             "https://x/a b", "https://x/?q", "https://x/#f", "https://x/.", "https://x/..",
                             "https://x/./a", "https://x/../a", "https://x//a", "HTTPS://x", "https://x_y",
                             "https://x/a%20b", " https://x", "https://x\n"})
        assert(insider_base_valid(base) == 0);
    assert(insider_base_valid(nullptr) == 0);
    const std::string longest = "https://" + std::string(INSIDER_MAX_BASE - 8, 'a');
    assert(insider_base_valid(longest.c_str()) == 1 && insider_base_valid((longest + "a").c_str()) == 0);

    char url[256];
    assert(insider_updates_url(url, sizeof(url), kInsider) != 0 && std::strcmp(url, kInsiderUpdates) == 0);
    assert(insider_updates_url(url, sizeof(url), "https://x/") == 0 && url[0] == '\0');
    assert(insider_updates_url(url, 20, kInsider) == 0 && url[0] == '\0');

    // The app's id, and the versions the service writes, as content versions.
    for (const char* app : {"nightshade", "dulcamara", "ab", "a-b", "x2", "a1234567890123456789012345678901"})
        assert(insider_app_valid(app) == 1);
    for (const char* app : {"", "a", "Nightshade", "1x", "-x", "a_b", "a b", "a12345678901234567890123456789012"})
        assert(insider_app_valid(app) == 0);
    char version[12];
    for (const auto& [text, content] : std::vector<std::pair<const char*, const char*>>{
             {"1.010.013", "01.010.013"}, {"v1.010.013", "01.010.013"}, {"01.010.013", "01.010.013"},
             {"1.10.13", "01.010.013"}, {"0.2.12", "00.002.012"}, {"99.999.999", "99.999.999"}}) {
        assert(insider_content_version(text, version) == 1 && std::strcmp(version, content) == 0);
    }
    for (const char* text : {"", "v", "1.010", "1.010.013.1", "1.1000.0", "100.0.0", "1.010.013-rc", "V1.010.013",
                             " 1.010.013", "1..013", "1.010.", "vv1.010.013", "1.010.0000013"}) {
        assert(insider_content_version(text, version) == 0 && version[0] == '\0');
    }

    // The request: the app, the version and the code, each a JSON string.
    char json[INSIDER_MAX_REQUEST + 64];
    assert(insider_request(json, sizeof(json), "nightshade", "01.010.012", "ABCDEFGH") != 0);
    assert(std::strcmp(json, "{\"app\":\"nightshade\",\"installed_version\":\"01.010.012\",\"code\":\"ABCDEFGH\"}") == 0);
    // Quotes, backslashes and control characters are escaped, whatever the text (the code sent is
    // always a valid one, but nothing can step out of its string).
    assert(insider_request(json, sizeof(json), "a\"b", "c\\d", "e\nf\x01g\x7f") != 0);
    assert(std::strcmp(json, "{\"app\":\"a\\\"b\",\"installed_version\":\"c\\\\d\",\"code\":\"e\\u000af\\u0001g\\u007f\"}") ==
           0);
    assert(insider_request(json, sizeof(json), "nightshade", "01.010.012", "\xc3\xa9") != 0 &&
           std::strstr(json, "\"code\":\"\xc3\xa9\"}") != nullptr);
    // Too small for it, over the service's 1 KB, or an argument missing: nothing.
    assert(insider_request(json, 40, "nightshade", "01.010.012", "ABCDEFGH") == 0 && json[0] == '\0');
    const std::string long_code(INSIDER_MAX_REQUEST, 'A');
    assert(insider_request(json, sizeof(json), "nightshade", "01.010.012", long_code.c_str()) == 0 && json[0] == '\0');
    assert(insider_request(json, sizeof(json), nullptr, "01.010.012", "ABCDEFGH") == 0);

    // The download's address: exactly <base>/v1/download/<token>, on the service's own host.
    const std::string good = InsiderUrl("v1.010.013");
    assert(self_update_insider_url(kInsider, good.c_str()) == 1);
    assert(self_update_insider_url("https://h.example/n", "https://h.example/n/v1/download/t") == 1);
    assert(self_update_insider_url("https://h.example/n", "https://h.example/v1/download/t") == 0);
    std::string longest_url = std::string(kInsider) + "/v1/download/";
    longest_url += std::string(SELF_UPDATE_INSIDER_MAX_URL - longest_url.size(), 'T');
    assert(self_update_insider_url(kInsider, longest_url.c_str()) == 1);
    for (const std::string& bad : std::vector<std::string>{
             longest_url + "T", "https://nightly-insider.oliv3rmoon.workers.dev.evil.example/v1/download/t",
             "https://evil.example/v1/download/t", "http://nightly-insider.oliv3rmoon.workers.dev/v1/download/t",
             std::string(kInsider) + "/v1/download/", std::string(kInsider) + "/v1/downloads/t",
             std::string(kInsider) + "/v1/updates", std::string(kInsider) + "/v1/download/t?x=1",
             std::string(kInsider) + "/v1/download/t#x", std::string(kInsider) + "/v1/download/t%2F",
             std::string(kInsider) + "/v1/download/a/../b", std::string(kInsider) + "/v1/download/..",
             std::string(kInsider) + "/v1/download/.", std::string(kInsider) + "/v1/download/a//b",
             std::string(kInsider) + "/v1/download/a/", std::string(kInsider) + "/v1/download/t\n",
             std::string(kInsider) + "/v1/download/t u", std::string(kInsider) + "@evil/v1/download/t",
             "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.010.013/Nightshade-v1.010.013.zip"})
        assert(self_update_insider_url(kInsider, bad.c_str()) == 0);
    assert(self_update_insider_url(nullptr, good.c_str()) == 0 && self_update_insider_url(kInsider, nullptr) == 0);
    assert(self_update_insider_url("http://x", "http://x/v1/download/t") == 0);

    // The kit downloads from there once the build names the service, and never as a redirect's
    // target; GitHub stays as it was. Without the service it is refused.
    assert(self_update_url_allowed(good.c_str(), 0) == 0);
    assert(self_update_allow_insider(kInsider) == 1);
    assert(self_update_url_allowed(good.c_str(), 0) == 1 && self_update_url_allowed(good.c_str(), 1) == 0);
    assert(self_update_url_allowed("https://evil.example/v1/download/token", 0) == 0);
    assert(self_update_url_allowed(kZip, 0) == 1 && self_update_url_allowed(kZip, 1) == 1);
    assert(self_update_allow_insider("") == 1 && self_update_url_allowed(good.c_str(), 0) == 0);
    assert(self_update_allow_insider("http://nightly-insider.oliv3rmoon.workers.dev") == 0 &&
           self_update_url_allowed("http://nightly-insider.oliv3rmoon.workers.dev/v1/download/token", 0) == 0);
    assert(self_update_allow_insider(nullptr) == 1 && self_update_url_allowed(good.c_str(), 0) == 0);
    assert(self_update_url_allowed(kZip, 0) == 1);
}

void InsiderAnswers() {
    static self_update_offer offer;
    insider_report report{};
    // "update": the build fills the offer as a GitHub test build's does; the log has its host and
    // file, never the address's token.
    const std::string update = InsiderUpdate("1.010.013");
    assert(insider_evaluate(update.data(), update.size(), 200, kInsider, "nightshade", "Nightshade", "PPSA99008",
                            "01.010.012", "01.010.012", &offer, &report) == INSIDER_UPDATE);
    assert(std::strcmp(offer.title, "PPSA99008") == 0 && std::strcmp(offer.name, "Nightshade") == 0);
    assert(std::strcmp(offer.installed, "01.010.012") == 0 && std::strcmp(offer.available, "01.010.013") == 0);
    assert(std::strcmp(offer.version, "v1.010.013") == 0 && offer.artifact == InsiderUrl("v1.010.013"));
    assert(std::strcmp(offer.sha256, kInsiderDigest) == 0 && offer.size == 81234567 && offer.beside[0] == '\0');
    assert(offer.prerelease == 1 && std::strcmp(offer.release_title, "Nightshade v1.010.013 (insider build)") == 0);
    assert(std::strcmp(offer.notes, "Insider build.\n- The launcher's new settings.") == 0 && !offer.notes_truncated);
    assert(offer.page[0] == '\0');
    assert(report.status == INSIDER_UPDATE && report.http_status == 200 && std::strcmp(report.answer, "update") == 0);
    assert(std::strcmp(report.tag, "v1.010.013") == 0 && std::strcmp(report.host, "nightly-insider.oliv3rmoon.workers.dev") == 0);
    assert(std::strcmp(report.file, "Nightshade-v1.010.013.zip") == 0 && report.why == nullptr);
    assert(Insider(Compact(update)) == INSIDER_UPDATE);
    // A title with a control character in it, none, and one too long; notes that are absent.
    assert(Insider(With(update, "\"Nightshade v1.010.013 (insider build)\"", "\"a\\tb\"")) == INSIDER_UPDATE);
    assert(insider_evaluate(update.data(), update.size(), 200, kInsider, "nightshade", "Nightshade", "PPSA99008",
                            "01.010.012", nullptr, &offer, &report) == INSIDER_UPDATE);
    {
        const std::string tabbed = With(update, "\"Nightshade v1.010.013 (insider build)\"", "\"a\\tb\"");
        assert(insider_evaluate(tabbed.data(), tabbed.size(), 200, kInsider, "nightshade", "Nightshade", "PPSA99008",
                                "01.010.012", nullptr, &offer, nullptr) == INSIDER_UPDATE &&
               std::strcmp(offer.release_title, "a b") == 0);
        const std::string untitled = With(With(update, "\"title\": \"Nightshade v1.010.013 (insider build)\",\n", ""),
                                          "\"notes\": \"Insider build.\\n- The launcher's new settings.\",\n", "");
        assert(insider_evaluate(untitled.data(), untitled.size(), 200, kInsider, "nightshade", "Nightshade",
                                "PPSA99008", "01.010.012", nullptr, &offer, nullptr) == INSIDER_UPDATE &&
               offer.release_title[0] == '\0' && offer.notes[0] == '\0');
        const std::string long_title =
            With(update, "\"Nightshade v1.010.013 (insider build)\"", "\"" + std::string(128, 'x') + "\"");
        assert(insider_evaluate(long_title.data(), long_title.size(), 200, kInsider, "nightshade", "Nightshade",
                                "PPSA99008", "01.010.012", nullptr, &offer, nullptr) == INSIDER_UPDATE &&
               offer.release_title[0] == '\0');
    }

    // Nothing for this member: the code is good with nothing newer, or it isn't (any more).
    for (const auto& [text, status] : std::vector<std::pair<const char*, insider_status>>{
             {"none", INSIDER_NONE}, {"not_qualified", INSIDER_NOT_QUALIFIED}, {"invalid_code", INSIDER_INVALID_CODE},
             {"expired_code", INSIDER_EXPIRED_CODE}}) {
        assert(Insider(InsiderNothing(text), 200, "01.010.012", nullptr, &report) == status);
        assert(report.status == status && std::strcmp(report.answer, text) == 0 && report.why == nullptr);
        assert(Insider(std::string("{\"status\":\"") + text + "\"}") == status);
    }
    // Another status or HTTP code: the service's errors, and "update" anywhere but in a 200.
    for (const auto& [http, body] : std::vector<std::pair<int, std::string>>{
             {400, "{\"status\":\"bad_request\"}"}, {404, "{\"status\":\"unknown_app\"}"}, {405, ""},
             {429, "{\"status\":\"rate_limited\"}"}, {503, "{\"status\":\"unavailable\"}"}, {201, update},
             // A redirect is not followed (the POST makes none): it is an answer like any other.
             {301, ""}, {302, update}, {307, update}, {308, "{\"status\":\"none\"}"},
             {500, "<html>error</html>"}, {200, "{\"status\":\"rate_limited\"}"}, {200, "{\"status\":\"Update\"}"}}) {
        assert(Insider(body, http, "01.010.012", nullptr, &report) == INSIDER_OTHER && report.http_status == http);
    }
    assert(Insider("{\"status\":\"rate_limited\"}", 429, "01.010.012", nullptr, &report) == INSIDER_OTHER &&
           std::strcmp(report.answer, "rate_limited") == 0);
    // JSON it can't read: not JSON, cut short, no status, a status that isn't a word, said twice.
    for (const std::string& body : std::vector<std::string>{
             "", "not json", "[]", "{}", "{\"status\":5}", "{\"status\":null}", "{\"status\":\"\"}",
             "{\"status\":\"up date\"}", "{\"status\":\"none\",\"status\":\"update\"}", "{\"status\":\"none\"",
             "{\"status\":\"none\"} x", update.substr(0, update.size() - 1), update.substr(0, update.size() / 2),
             "{\"status\":\"" + std::string(40, 'a') + "\"}"}) {
        assert(Insider(body, 200, "01.010.012", nullptr, &report) == INSIDER_BAD_RESPONSE && report.answer[0] == '\0');
    }

    // "update" with a build that fails a check is not offered: the download's address elsewhere...
    const std::string url = InsiderUrl("v1.010.013");
    for (const std::string& elsewhere : std::vector<std::string>{
             "https://evil.example/v1/download/token", "http://nightly-insider.oliv3rmoon.workers.dev/v1/download/t",
             std::string(kInsider) + ".evil.example/v1/download/t", std::string(kInsider) + "/v1/updates",
             std::string(kInsider) + "/v1/download/t?sig=1", std::string(kInsider) + "/v1/download/a/../b",
             std::string(kInsider) + "/v1/download/" + std::string(SELF_UPDATE_INSIDER_MAX_URL, 't'),
             "https://github.com/Oliv3rmoon/Nightshade/releases/download/v1.010.013/Nightshade-v1.010.013.zip"}) {
        assert(std::strcmp(Refused(With(update, url, elsewhere)), "url") == 0);
    }
    assert(std::strcmp(Refused(With(update, "\"url\": \"" + url + "\",\n", "")), "url") == 0);
    // ... a SHA-256 that isn't 64 lowercase hexadecimal digits ...
    for (const std::string& digest : std::vector<std::string>{
             "5D41402ABC4B2A76B9719D911017C592AAE4B6E1B2C3D4E5F60718293A4B5C6D", std::string(63, 'a'),
             std::string(65, 'a'), std::string(63, 'a') + "g", "", std::string(64, ' ')}) {
        assert(std::strcmp(Refused(InsiderUpdate("1.010.013", digest)), "sha256") == 0);
    }
    assert(std::strcmp(Refused(With(update, "\"sha256\"", "\"sha512\"")), "sha256") == 0);
    // ... a size of nothing, past the kit's limit, or not a whole number ...
    assert(std::strcmp(Refused(InsiderUpdate("1.010.013", kInsiderDigest, 0)), "size") == 0);
    assert(std::strcmp(Refused(InsiderUpdate("1.010.013", kInsiderDigest, SELF_UPDATE_MAX_ARCHIVE + 1)), "size") == 0);
    assert(Insider(InsiderUpdate("1.010.013", kInsiderDigest, SELF_UPDATE_MAX_ARCHIVE)) == INSIDER_UPDATE);
    for (const char* size : {"\"81234567\"", "-1", "8.1e7", "81234567.0", "081234567", "null", "99999999999999999999"})
        assert(std::strcmp(Refused(With(update, "81234567", size)), "size") == 0);
    // ... a version that isn't newer than the install or the program, or on another line ...
    assert(std::strcmp(Refused(InsiderUpdate("1.010.012")), "not-newer") == 0);
    assert(std::strcmp(Refused(InsiderUpdate("1.010.011")), "not-newer") == 0);
    for (const char* other : {"1.011.000", "1.012.001", "2.010.013", "1.009.999"})
        assert(std::strcmp(Refused(InsiderUpdate(other)), "another-line") == 0);
    assert(std::strcmp(Refused(InsiderUpdate("1.010.012"), "01.010.010", "01.010.012"), "not-newer") == 0);
    assert(std::strcmp(Refused(InsiderUpdate("1.011.001"), "01.011.000", "01.010.012"), "another-line") == 0);
    assert(Insider(InsiderUpdate("1.010.013"), 200, "01.010.010", "01.010.012") == INSIDER_UPDATE);
    // ... a version or a tag that can't be read, another app's answer, no build.
    for (const char* version : {"\"1.010\"", "\"1.010.013-rc1\"", "10", "\"\""})
        assert(std::strcmp(Refused(With(update, "\"1.010.013\"", version)), "version") == 0);
    for (const char* tag : {"\"v1.010.013 x\"", "\"\"", "\"v1/010\"", "\"v1.010.013\\n\"", "13"})
        assert(std::strcmp(Refused(With(update, "\"v1.010.013\"", tag)), "tag") == 0);
    assert(std::strcmp(Refused(With(update, "\"v1.010.013\"", "\"" + std::string(40, 'v') + "\"")), "tag") == 0);
    assert(std::strcmp(Refused(With(update, "\"app\": \"nightshade\"", "\"app\": \"dulcamara\"")), "app") == 0);
    assert(std::strcmp(Refused(With(update, "\"app\": \"nightshade\"", "\"app\": 1")), "app") == 0);
    assert(Insider(With(update, "\"app\": \"nightshade\",\n", "")) == INSIDER_UPDATE);
    assert(std::strcmp(Refused("{\"status\":\"update\"}"), "build") == 0);
    assert(std::strcmp(Refused("{\"status\":\"update\",\"build\":[]}"), "build") == 0);
    assert(std::strcmp(Refused("{\"status\":\"update\",\"build\":null}"), "build") == 0);
    // A field of the build said twice is not a build that can be read.
    assert(std::strcmp(Refused(With(update, "\"size\": 81234567", "\"size\": 81234567, \"size\": 1")), "size") == 0);
    assert(std::strcmp(Refused(With(update, "\"url\": ", "\"url\": \"" + url + "\", \"url\": ")), "url") == 0);
    // Nothing to read with.
    assert(insider_evaluate(update.data(), update.size(), 200, kInsider, "nightshade", "Nightshade", "PPSA99008",
                            nullptr, nullptr, &offer, &report) == INSIDER_BAD_ARGUMENT);
}

void InsiderRequests() {
    static self_update_offer offer;
    insider_report report{};
    // The request: POST /v1/updates with the app, the program's version and the code, the app's
    // User-Agent; nothing else.
    Answer(200, InsiderUpdate("1.010.013"));
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                         "01.010.012", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(service.asked.size() == 1 && service.asked[0].url == kInsiderUpdates);
    assert(service.asked[0].agent == "Nightshade/01.010.012");
    assert(service.asked[0].json == "{\"app\":\"nightshade\",\"installed_version\":\"01.010.012\",\"code\":\"ABCDEFGH\"}");
    assert(report.status == INSIDER_UPDATE && std::strcmp(offer.available, "01.010.013") == 0);
    // The program's version is what is sent; the install's (param.json) is what the offer says.
    Answer(200, InsiderUpdate("1.010.013"));
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.011",
                         "01.010.012", &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(service.asked[0].agent == "Nightshade/01.010.012" && service.asked[0].json.find("01.010.012") != std::string::npos);
    assert(std::strcmp(offer.installed, "01.010.011") == 0);
    Answer(200, InsiderUpdate("1.010.013"));
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.011",
                         nullptr, &offer, &report) == SELF_UPDATE_AVAILABLE);
    assert(service.asked[0].agent == "Nightshade/01.010.011");

    // Nothing is sent without a valid code, service, app or version.
    for (const char* code : {"", "abcdefgh", "ABCD-EFGH", "ABCDEFG", static_cast<const char*>(nullptr)}) {
        Answer(200, InsiderUpdate("1.010.013"));
        assert(insider_check(Post, &service, kInsider, code, "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                             nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN);
        assert(service.asked.empty() && report.status == INSIDER_BAD_ARGUMENT && offer.artifact[0] == '\0');
    }
    for (const char* base : {"", "http://x", "https://x/", static_cast<const char*>(nullptr)}) {
        Answer(200, InsiderUpdate("1.010.013"));
        assert(insider_check(Post, &service, base, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                             nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN && service.asked.empty());
    }
    Answer(200, InsiderUpdate("1.010.013"));
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "Nightshade", "Nightshade", "PPSA99008", "01.010.012",
                         nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN && service.asked.empty());
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "development",
                         nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN && service.asked.empty());
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                         "v1.010.012", &offer, &report) == SELF_UPDATE_UNKNOWN && service.asked.empty());
    assert(insider_check(nullptr, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008",
                         "01.010.012", nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN);

    // No answer, a timeout, an answer too large: silence, never an offer.
    Answer(200, InsiderUpdate("1.010.013"));
    service.result = -10028; // CURLE_OPERATION_TIMEDOUT
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                         nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN);
    assert(report.status == INSIDER_NETWORK && report.platform_error == -10028 && report.http_status == 0 &&
           offer.artifact[0] == '\0' && service.asked.size() == 1);
    Answer(200, InsiderUpdate("1.010.013") + std::string(INSIDER_MAX_ANSWER, ' '));
    assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008", "01.010.012",
                         nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN);
    assert(report.status == INSIDER_TOO_LARGE && report.http_status == 200 && offer.artifact[0] == '\0');
    // An answer as long as the notes the service allows (4,000 characters, three bytes each) fits.
    {
        std::string notes;
        for (int i = 0; i < 4000; ++i) notes += "\xe6\xbc\xa2";
        Answer(200, With(InsiderUpdate("1.010.013"), "Insider build.\\n- The launcher's new settings.", notes));
        assert(service.body.size() < INSIDER_MAX_ANSWER);
        assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008",
                             "01.010.012", nullptr, &offer, &report) == SELF_UPDATE_AVAILABLE &&
               offer.notes == notes);
    }
    for (const char* status : {"none", "not_qualified", "invalid_code", "expired_code"}) {
        Answer(200, InsiderNothing(status));
        assert(insider_check(Post, &service, kInsider, "ABCDEFGH", "nightshade", "Nightshade", "PPSA99008",
                             "01.010.012", nullptr, &offer, &report) == SELF_UPDATE_UNKNOWN);
        assert(std::strcmp(insider_status_text(report.status), status) == 0 && offer.artifact[0] == '\0');
    }
}

// Test builds with insider builds: the service first, GitHub's test builds whenever it offers nothing.
self_update_check_result InsiderTestCheck(const char* code, self_update_offer* offer, insider_report* insider,
                                          github_release_report* github, const char* base = kInsider) {
    network.asked.clear();
    return insider_check_tests_from(&kPlatform, Post, &service, base, code, "nightshade", kRepository, "Nightshade",
                                    "PPSA99008", "01.010.007", "01.010.007", offer, insider, github);
}

void InsiderFallback() {
    static self_update_offer offer;
    insider_report insider{};
    github_release_report github{};
    const std::string list = ListOf(Repository());

    // The insider build wins when the service offers one: GitHub is not asked.
    ServeList(list);
    Answer(200, InsiderUpdate("1.010.009"));
    assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github) == SELF_UPDATE_AVAILABLE);
    assert(insider.status == INSIDER_UPDATE && service.asked.size() == 1 && network.asked.empty());
    assert(std::strcmp(offer.available, "01.010.009") == 0 && offer.artifact == InsiderUrl("v1.010.009"));
    assert(std::strcmp(offer.installed, "01.010.007") == 0 && offer.prerelease == 1);

    // An expired code: the test build GitHub has is offered, exactly as without insider builds,
    // and the settings will say where to get a new code.
    static self_update_offer without;
    github_release_report without_report{};
    ServeList(list);
    assert(TestCheck(kRepository, "01.010.007", &without, &without_report) == SELF_UPDATE_AVAILABLE);
    const std::vector<std::string> github_requests = network.asked;
    assert((github_requests == std::vector<std::string>{kList, SumsUrl("v1.010.008")}));
    for (const char* status : {"expired_code", "invalid_code", "not_qualified", "none"}) {
        ServeList(list);
        Answer(200, InsiderNothing(status));
        assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github) == SELF_UPDATE_AVAILABLE);
        assert(std::strcmp(insider_status_text(insider.status), status) == 0 && service.asked.size() == 1);
        assert(network.asked == github_requests && github.reason == GITHUB_RELEASE_NEWER && github.prerelease == 1);
        assert(std::strcmp(offer.version, "v1.010.008") == 0 && offer.artifact == ZipUrl("v1.010.008"));
        assert(offer.sha256 == DigestOf("v1.010.008") && std::memcmp(&offer, &without, sizeof(offer)) == 0);
        assert(insider_status_hints(insider.status) == (std::strcmp(status, "none") != 0));
    }
    // Offline (the request fails), a timeout, the service's errors, an answer it can't read, an
    // update it refuses: the same.
    for (int failure : {-10006, -10007, -10028, -10035}) {
        ServeList(list);
        Answer(200, InsiderUpdate("1.010.009"));
        service.result = failure;
        assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github) == SELF_UPDATE_AVAILABLE);
        assert(insider.status == INSIDER_NETWORK && insider.platform_error == failure && !insider_status_hints(insider.status));
        assert(network.asked == github_requests && std::memcmp(&offer, &without, sizeof(offer)) == 0);
    }
    for (const auto& [http, body] : std::vector<std::pair<int, std::string>>{
             {429, "{\"status\":\"rate_limited\"}"}, {503, "{\"status\":\"unavailable\"}"}, {200, "garbage"},
             {302, InsiderUpdate("1.010.009")}, {307, InsiderUpdate("1.010.009")},
             {200, InsiderUpdate("1.010.009").substr(0, 200)}, {200, InsiderUpdate("1.010.009", std::string(64, 'X'))},
             {200, With(InsiderUpdate("1.010.009"), InsiderUrl("v1.010.009"), "https://evil.example/v1/download/x")},
             {200, InsiderUpdate("1.011.000")}, {200, InsiderUpdate("1.010.007")}}) {
        ServeList(list);
        Answer(http, body);
        assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github) == SELF_UPDATE_AVAILABLE);
        assert(insider.status != INSIDER_UPDATE && network.asked == github_requests);
        assert(std::memcmp(&offer, &without, sizeof(offer)) == 0);
    }
    // No code kept, or no service named: nothing is sent, GitHub as always.
    for (const char* none : {"", static_cast<const char*>(nullptr)}) {
        ServeList(list);
        Answer(200, InsiderUpdate("1.010.009"));
        assert(InsiderTestCheck(none, &offer, &insider, &github) == SELF_UPDATE_AVAILABLE);
        assert(insider.status == INSIDER_NOT_ASKED && service.asked.empty() && network.asked == github_requests);
        ServeList(list);
        assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github, none) == SELF_UPDATE_AVAILABLE);
        assert(insider.status == INSIDER_NOT_ASKED && service.asked.empty() && network.asked == github_requests);
    }
    // GitHub has nothing either: nothing at all, never an error.
    ServeList("[]");
    Answer(200, InsiderNothing("expired_code"));
    assert(InsiderTestCheck("ABCDEFGH", &offer, &insider, &github) == SELF_UPDATE_UNKNOWN);
    assert(insider.status == INSIDER_EXPIRED_CODE && github.reason == GITHUB_RELEASE_NONE && offer.artifact[0] == '\0');
}

// A helper stood in for (headless/self_update_helper): it takes the request, says it is ready,
// counts what it is sent, and stages the release only when the app's last piece (the end mark)
// arrives; then it waits for the go-ahead or a cancel.
struct FakeHelper {
    int fd = -1;
    std::thread thread;
    std::string request;
    std::uint64_t received = 0;
    bool ended = false;  // the end mark came: the release was handed over whole
    std::string after;   // what the app said then ("cancel\n", "apply\n")
};
FakeHelper* helper = nullptr;

bool ReadAll(int fd, void* data, std::size_t size) {
    char* bytes = static_cast<char*>(data);
    while (size != 0) {
        const ssize_t count = ::read(fd, bytes, size);
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

void RunHelper(FakeHelper* fake) {
    for (int lines = 0; lines < 9;) {
        char byte = 0;
        if (!ReadAll(fake->fd, &byte, 1)) return;
        fake->request += byte;
        if (byte == '\n') ++lines;
    }
    if (::write(fake->fd, "ready\n", 6) != 6) return;
    std::vector<char> piece;
    for (;;) {
        unsigned char header[4];
        if (!ReadAll(fake->fd, header, sizeof(header))) return;
        const std::size_t size = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<std::size_t>(header[3]) << 24);
        if (size == 0) break;
        piece.resize(size);
        if (!ReadAll(fake->fd, piece.data(), size)) return;
        fake->received += size;
    }
    fake->ended = true;
    if (::write(fake->fd, "staged\n", 7) != 7) return;
    char byte = 0;
    while (ReadAll(fake->fd, &byte, 1)) fake->after += byte;
}

long HelperSend(void* user, const void* data, std::size_t size) {
    return static_cast<long>(::write(static_cast<int>(reinterpret_cast<std::intptr_t>(user)), data, size));
}
long HelperReceive(void* user, void* data, std::size_t size) {
    return static_cast<long>(::read(static_cast<int>(reinterpret_cast<std::intptr_t>(user)), data, size));
}
void HelperClose(void* user) { ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(user))); }

int OpenFakeHelper(void*, self_update_channel* channel) {
    int ends[2];
    if (helper == nullptr || socketpair(AF_UNIX, SOCK_STREAM, 0, ends) != 0) return 0;
    helper->fd = ends[1];
    helper->thread = std::thread(RunHelper, helper);
    channel->send = HelperSend;
    channel->receive = HelperReceive;
    channel->close = HelperClose;
    channel->user = reinterpret_cast<void*>(static_cast<std::intptr_t>(ends[0]));
    return 1;
}

std::uint64_t Clock(void*) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// The kit's job for an offer, as the launcher follows it, until it is staged or has ended.
self_update_status RunJob(const self_update_offer& offer, FakeHelper& fake, bool cancel_when_staged) {
    static const self_update_platform platform = {Fetch, Download, Verify, OpenFakeHelper, LoadSequence,
                                                  SaveSequence, Clock, nullptr};
    static self_update_job job;
    self_update_status status{};
    std::memset(&job, 0, sizeof(job));
    helper = &fake;
    assert(self_update_start(&job, &platform, &offer) == 1);
    for (int wait = 0; wait < 20000; ++wait) {
        self_update_poll(&job, &status);
        if (status.phase == SELF_UPDATE_READY || status.phase == SELF_UPDATE_FAILED ||
            status.phase == SELF_UPDATE_CANCELLED)
            break;
        usleep(1000);
    }
    if (status.phase == SELF_UPDATE_READY && cancel_when_staged) self_update_cancel(&job);
    self_update_finish(&job);
    if (fake.thread.joinable()) fake.thread.join();
    if (fake.fd >= 0) ::close(fake.fd);
    helper = nullptr;
    return status;
}

std::string Sha256Of(const std::string& bytes) {
    self_update_sha256 hash;
    unsigned char digest[32];
    char text[65];
    self_update_sha256_start(&hash);
    self_update_sha256_add(&hash, bytes.data(), bytes.size());
    self_update_sha256_finish(&hash, digest);
    self_update_sha256_hex(digest, text);
    return text;
}

void InsiderDownload() {
    std::signal(SIGPIPE, SIG_IGN); // a write to a connection the other side closed fails instead
    static self_update_offer offer;
    std::string release(300000, '\0');
    for (std::size_t i = 0; i < release.size(); ++i) release[i] = static_cast<char>((i * 31 + 7) & 0xff);
    const std::string answer = InsiderUpdate("1.010.013", Sha256Of(release), release.size());
    assert(insider_evaluate(answer.data(), answer.size(), 200, kInsider, "nightshade", "Nightshade", "PPSA99008",
                            "01.010.012", "01.010.012", &offer, nullptr) == INSIDER_UPDATE);
    assert(self_update_allow_insider(kInsider) == 1);

    // The build as the service signed it: downloaded from its address, checked, handed over whole
    // and staged; the helper is told what any update tells it. (Cancelled here, nothing applied.)
    {
        network = Network{};
        network.files[offer.artifact] = release;
        FakeHelper fake;
        const self_update_status status = RunJob(offer, fake, true);
        assert(status.phase == SELF_UPDATE_READY && fake.ended && fake.received == release.size());
        assert(fake.request == std::string(SELF_UPDATE_MAGIC) + "\nupdate\nPPSA99008\n01.010.012\n01.010.013\n" +
                                   std::to_string(release.size()) + "\n" + Sha256Of(release) +
                                   "\nNightshade\nv1.010.013\n");
        assert(fake.after == "cancel\n");
        assert((network.asked == std::vector<std::string>{offer.artifact}));
    }
    // A tampered download (the same size, other bytes, so a SHA-256 that differs from the one the
    // service gave): refused before the helper gets the end mark, so nothing is staged or installed.
    {
        std::string tampered = release;
        tampered[tampered.size() / 2] ^= 0x01;
        network = Network{};
        network.files[offer.artifact] = tampered;
        FakeHelper fake;
        const self_update_status status = RunJob(offer, fake, false);
        assert(status.phase == SELF_UPDATE_FAILED);
        assert(std::strcmp(status.error, "The download doesn't match the catalog's listing") == 0);
        assert(!fake.ended && fake.after.empty() && fake.received == tampered.size());
    }
    // One byte more or less than the build's size: refused as well.
    for (const std::string& resized : {release.substr(0, release.size() - 1), release + "x"}) {
        network = Network{};
        network.files[offer.artifact] = resized;
        FakeHelper fake;
        const self_update_status status = RunJob(offer, fake, false);
        assert(status.phase == SELF_UPDATE_FAILED && !fake.ended && fake.after.empty());
    }
    // The address refused (403: it expired, or the member's code was removed since the answer):
    // the download fails as any other, and nothing is staged.
    {
        network = Network{};
        network.files[offer.artifact] = release;
        network.failure = -403; // as console_download answers a status that isn't 200
        FakeHelper fake;
        const self_update_status status = RunJob(offer, fake, false);
        assert(status.phase == SELF_UPDATE_FAILED && std::strcmp(status.error, "The download failed") == 0);
        assert(!fake.ended && fake.after.empty() && fake.received == 0);
    }
    // A build without the service named: its address is not one the kit downloads from.
    assert(self_update_allow_insider(nullptr) == 1);
    {
        network = Network{};
        network.files[offer.artifact] = release;
        FakeHelper fake;
        const self_update_status status = RunJob(offer, fake, false);
        assert(status.phase == SELF_UPDATE_FAILED && std::strcmp(status.error, "The download failed") == 0);
        assert(!fake.ended && fake.received == 0);
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
    // The same for the insider service's address (EDEN_INSIDER_URL, headless/insider_url.cmake).
    if (argc == 3 && std::strcmp(argv[1], "--insider-url") == 0) {
        std::printf("%d\n", insider_base_valid(argv[2]));
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
    InsiderCodes();
    InsiderAddresses();
    InsiderAnswers();
    InsiderRequests();
    InsiderFallback();
    InsiderDownload();
    std::puts("update check: versions, request address, newer/same/older and silent answers PASS");
    std::puts("update check from GitHub: tag versions, addresses and redirects, SHA256SUMS lines, release notes, "
              "releases/latest answers, the ZIP SHA256SUMS lists, the check, its redirects and silent answers, no "
              "repository is the catalog PASS");
    std::puts("update check with test builds: version lines, the list's address and redirect, the release chosen "
              "(this line only, highest version, pre-release or not, drafts passed over), the list read whole, the "
              "test build's title, its SHA256SUMS and ZIP, without test builds releases/latest alone PASS");
    std::puts("insider builds: the code normalised, checked and kept, the service's address, the request and its "
              "escaping, update/none/not_qualified/invalid_code/expired_code, other statuses, answers cut short or "
              "unreadable, builds refused (address, SHA-256, size, version, line, tag, app), GitHub's test builds "
              "after any answer but an offer and offline, the insider build first when offered, a download whose "
              "SHA-256 or size differs, or that is refused (403), failed with nothing staged PASS");
    return 0;
}
