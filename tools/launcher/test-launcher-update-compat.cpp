// SPDX-License-Identifier: GPL-3.0-or-later
// The self-update helper's compatibility with what it is given (tools/check-update-helper.py).
//
//   test-launcher-update-compat <empty folder> [--release-layout]
//
// The launcher packs an update keeps, the bounded JSON readers and the wait for the app to close;
// then the release ZIP as it is since Nightshade has its own title ID (tools/ci/make-dist.py): the
// app as PPSA64448/ and the bridge as PPSA99008/, at a version above every PPSA99008 release. An
// install made before (PPSA99008, up to 1.010) takes PPSA99008/ as its update; Nightshade's own
// takes PPSA64448/. --release-layout runs that part alone, with the API every released helper has:
// check-update-helper.py builds it with the helpers 1.005.102 to 1.010.003 shipped as well.
#include "updater.hpp"
#include "update_check.h"
#include "self_update_sha256.h"
#include "miniz/miniz.h"
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
namespace fs=std::filesystem;
static void write(const fs::path& path,const std::string& text) {fs::create_directories(path.parent_path());std::ofstream(path,std::ios::binary)<<text;}
static std::string read(const fs::path& path) {std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
static std::string param(const std::string& title,const std::string& version) {return "{\"titleId\": \""+title+"\", \"contentVersion\": \""+version+"\"}";}

// A ZIP of these files, as tools/ci/make-dist.py lays it out.
static std::string zip_of(const std::vector<std::pair<std::string,std::string>>& files) {
    mz_zip_archive zip{};mz_zip_writer_init_heap(&zip,0,0);
    for(const auto& [name,body]:files) mz_zip_writer_add_mem(&zip,name.c_str(),body.data(),body.size(),MZ_DEFAULT_COMPRESSION);
    void* buffer=nullptr;std::size_t size=0;mz_zip_writer_finalize_heap_archive(&zip,&buffer,&size);
    std::string out(static_cast<const char*>(buffer),size);mz_free(buffer);mz_zip_writer_end(&zip);return out;
}
// The release: Nightshade (PPSA64448/) and the bridge (PPSA99008/), each at its version, with the
// README beside them. The bridge carries launcher-runtime.json as the app does, so that a helper
// keeping a selected launcher pack (launcher_update_compatible) takes it as it takes the app.
static std::string release_zip(const std::string& app_version,const std::string& bridge_version,const std::string& runtime) {
    return zip_of({{"PPSA64448/eboot.bin","nightshade "+app_version},{"PPSA64448/sce_sys/param.json",param("PPSA64448",app_version)},
                   {"PPSA64448/sce_module/libc.prx","libc"},{"PPSA64448/ui/art/backdrop.tga","art"},
                   {"PPSA64448/launcher-runtime.json",runtime},{"PPSA64448/lapy.elf","lapy"},
                   {"PPSA99008/eboot.bin","bridge "+bridge_version},{"PPSA99008/sce_sys/param.json",param("PPSA99008",bridge_version)},
                   {"PPSA99008/sce_module/libc.prx","libc"},{"PPSA99008/ui/lang/de-DE.po","msgid \"\""},
                   {"PPSA99008/launcher-runtime.json",runtime},{"PPSA99008/self-updater.elf","helper"},
                   {"README.md","readme"},{"LICENSE","license"}});
}
static std::string sha256(const std::string& data) {
    self_update_sha256 hash;unsigned char digest[32];char text[65];
    self_update_sha256_start(&hash);self_update_sha256_add(&hash,data.data(),data.size());
    self_update_sha256_finish(&hash,digest);self_update_sha256_hex(digest,text);return text;
}
// The app's side of the conversation, as self_update.c speaks it for an update: the request, the
// archive once the helper is ready, "apply" once it is staged, then it closes (the app ends).
struct AppSide {
    std::mutex lock;std::condition_variable wake;std::string inbox,line,archive;bool ended=false;std::vector<std::string> heard;
    void push(const std::string& bytes) {inbox+=bytes;wake.notify_all();}
    void end() {ended=true;wake.notify_all();}
    long read(void* data,std::size_t size) {
        std::unique_lock guard(lock);wake.wait(guard,[&]{return !inbox.empty()||ended;});
        if(inbox.empty()) return 0;
        const std::size_t count=std::min(size,inbox.size());std::memcpy(data,inbox.data(),count);inbox.erase(0,count);return static_cast<long>(count);
    }
    long write(const void* data,std::size_t size) {
        std::lock_guard guard(lock);
        for(std::size_t i=0;i<size;++i) {
            const char c=static_cast<const char*>(data)[i];
            if(c!='\n') {line.push_back(c);continue;}
            if(line.rfind("p ",0)!=0) {
                heard.push_back(line);
                if(line=="ready") {
                    for(std::size_t at=0;at<archive.size();at+=65536) {
                        const std::size_t piece=std::min<std::size_t>(65536,archive.size()-at);
                        const char header[4]={static_cast<char>(piece),static_cast<char>(piece>>8),static_cast<char>(piece>>16),static_cast<char>(piece>>24)};
                        push(std::string(header,4)+archive.substr(at,piece));
                    }
                    push(std::string(4,'\0'));
                } else if(line=="staged") push("apply\n");
                else if(line=="applying"||line.rfind("fail ",0)==0) end();
            }
            line.clear();
        }
        return static_cast<long>(size);
    }
    std::string last() {std::lock_guard guard(lock);return heard.empty()?std::string{}:heard.back();}
};
// The helpers' listeners can outlive their conversations: the app's sides stay until the end.
static std::vector<std::unique_ptr<AppSide>> sides;
// One update of `title` from `installed` to `available` with this archive, in a console made in
// `base`: /data/homebrew holds the apps, /data/prosperoeden is the data folder of every release up
// to 1.010 (the helper's previous-version and launcher folder there), /mnt/sandbox none.
struct Console {
    fs::path base,data,homebrew;self_update::Environment environment;std::vector<std::string> notices;
    explicit Console(const fs::path& where):base(where),data(where/"data"),homebrew(where/"data/homebrew") {
        fs::remove_all(base);fs::create_directories(homebrew);fs::create_directories(base/"sandbox");fs::create_directories(base/"user");
        environment.roots={homebrew.string()};environment.drives={data.string()};
        environment.sandboxes=(base/"sandbox").string();environment.registered=(base/"user").string();
        environment.sleep=[](unsigned){usleep(1000);};
        environment.notify=[this](const std::string& message){notices.push_back(message);};
        environment.previous=(data/"prosperoeden/previous-version").string();
        environment.launcher_data=(data/"prosperoeden").string();
        environment.settle_ms=0;
        fs::create_directories(data/"prosperoeden");
    }
    std::pair<int,std::string> update(const std::string& title,const std::string& installed,const std::string& available,const std::string& archive,const std::string& digest="") {
        sides.push_back(std::make_unique<AppSide>());AppSide& app=*sides.back();app.archive=archive;
        for(const std::string& line:{std::string{"PSU1"},std::string{"update"},title,installed,available,std::to_string(archive.size()),
                                     digest.empty()?sha256(archive):digest,std::string{"Nightshade"},"v"+available.substr(1)})
            app.push(line+"\n");
        const self_update::Io io{[&app](void* bytes,std::size_t size){return app.read(bytes,size);},
                                 [&app](const void* bytes,std::size_t size){return app.write(bytes,size);}};
        const int result=self_update::run(environment,io);
        {std::lock_guard guard(app.lock);app.end();}
        return {result,app.last()};
    }
};

// What every released helper does with the release ZIP that carries the bridge.
static void release_layout(const fs::path& root,const std::string& runtime,int& checks) {
    auto need=[&](bool ok,const char* what){if(!ok) {std::cerr<<"FAIL release layout: "<<what<<"\n";std::exit(1);}++checks;};
    const std::string release=release_zip("01.010.004","01.010.004",runtime);
    {
        // An install made before Nightshade had its own title ID, at the last such release, with the
        // player's files in its folder and a launcher pack selected.
        Console console(root/"old-install");
        const fs::path old=console.homebrew/"PPSA99008";
        write(old/"eboot.bin","nightshade 01.010.003");write(old/"sce_sys/param.json",param("PPSA99008","01.010.003"));
        write(old/"ui/art/backdrop.tga","old art");write(old/"lapy.elf","old lapy");write(old/"language.txt","de-DE");
        write(console.data/"prosperoeden/config/launcher.json",R"({"selected":"sample"})");
        write(console.data/"prosperoeden/launchers/sample/manifest.json",R"({"host_contract":"launcher-v1.0"})");
        const auto [result,said]=console.update("PPSA99008","01.010.003","01.010.004",release);
        need(result==0 && said=="applying","an old install updates to the release that carries the bridge");
        need(read(old/"eboot.bin")=="bridge 01.010.004" && read(old/"sce_sys/param.json")==param("PPSA99008","01.010.004"),
             "an old install takes PPSA99008/: the bridge, at its version");
        need(read(old/"self-updater.elf")=="helper" && read(old/"ui/lang/de-DE.po")=="msgid \"\"" && !fs::exists(old/"ui/art"),
             "the bridge's folders replace the old app's of the same name");
        need(read(old/"language.txt")=="de-DE" && read(old/"lapy.elf")=="old lapy",
             "what the bridge doesn't have stays (the helper replaces only the release's entries)");
        need(!fs::exists(console.homebrew/"PPSA64448") && !fs::exists(old/"PPSA64448") && !fs::exists(old/"README.md"),
             "nothing of PPSA64448/ or beside it is put anywhere by an old install's update");
        need(read(console.data/"prosperoeden/previous-version/PPSA99008-backup/eboot.bin")=="nightshade 01.010.003",
             "the old app is kept as the previous version, as every update keeps it");
        need(!fs::exists(console.data/"self-update"),"no work folder is left");
    }
    {
        // The version must be above the installed one, and the folder's the one offered.
        Console console(root/"versions");
        const fs::path old=console.homebrew/"PPSA99008";
        write(old/"eboot.bin","nightshade 01.010.003");write(old/"sce_sys/param.json",param("PPSA99008","01.010.003"));
        const std::string same=release_zip("01.010.003","01.010.003",runtime);
        need(console.update("PPSA99008","01.010.003","01.010.003",same).second=="fail The request was refused",
             "a release at the installed version is refused before anything is sent");
        const std::string behind=release_zip("01.010.004","01.010.003",runtime);
        need(console.update("PPSA99008","01.010.003","01.010.004",behind).second=="fail The download isn't the version the catalog lists" &&
                 read(old/"eboot.bin")=="nightshade 01.010.003",
             "a bridge at another version than the release's is refused, and nothing changes");
    }
    {
        // Nightshade's own update takes PPSA64448/ from the same ZIP.
        Console console(root/"own");
        const fs::path app=console.homebrew/"PPSA64448";
        console.environment.previous=(console.data/"nightshade/previous-version").string();
        console.environment.launcher_data=(console.data/"nightshade").string();
        write(app/"eboot.bin","nightshade 01.010.003");write(app/"sce_sys/param.json",param("PPSA64448","01.010.003"));
        const auto [result,said]=console.update("PPSA64448","01.010.003","01.010.004",release);
        need(result==0 && said=="applying" && read(app/"eboot.bin")=="nightshade 01.010.004" &&
                 read(app/"ui/art/backdrop.tga")=="art" && !fs::exists(app/"self-updater.elf") && !fs::exists(console.homebrew/"PPSA99008"),
             "Nightshade's own update takes PPSA64448/ and nothing of the bridge");
    }
}

int main(int argc,char** argv) {
    assert(argc==2||(argc==3 && std::string{argv[2]}=="--release-layout"));
    const fs::path root=argv[1],data=root/"data",stage=root/"staged";
    std::string reason;int checks=0;
    const std::string runtime=R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})";
    if(argc==3) {
        release_layout(root,runtime,checks);
        std::cout<<"PASS: "<<checks<<" checks of the release ZIP with PPSA64448/ and PPSA99008/\n";
        return 0;
    }
    auto check=[&](bool expected) {assert(self_update::launcher_update_compatible(data.string(),stage.string(),reason)==expected);++checks;};
    check(true);write(data/"config/launcher.json",R"({"selected":"builtin"})");check(true);
    write(data/"config/launcher.json",R"({"selected":"sample"})");
    const auto manifest=data/"launchers/sample/manifest.json",descriptor=stage/"launcher-runtime.json";
    write(manifest,R"({"host_contract":"launcher-v1.0"})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0"})");check(true);
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":1},"required_capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":0,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":[]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(true);
    write(descriptor,R"({"supports":"launcher-v2.0,launcher-v1.0","api_major":1,"api_minor":2,"capabilities":["extra.v1","updates.launcher-packs.v1"]})");check(true);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":2,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":-1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":"updates.launcher-packs.v1"})");check(false);
    write(descriptor,R"({"supports":"launcher-v2.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"api_minor":0,"capabilities":["updates.launcher-packs.v1"]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1",]})");check(false);
    write(descriptor,R"({"supports":"launcher-v1.0","api_major":1,"api_minor":1,"capabilities":["updates.launcher-packs.v1"]})");
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":0},"required_capabilities":[]})");check(true);
    write(manifest,R"({"host_contract":"launcher-v1.0","api":{"major":1,"min_minor":1e0}})");check(false);
    write(data/"config/launcher.json",R"({"selected":"../escape"})");check(false);
    unsigned number=0;const char* value=nullptr;size_t length=0;char text[64]{};
    auto need=[&](bool ok){assert(ok);++checks;};
    std::string json=R"({"value":4294967295,"object":{"a":["b"]}})";
    need(update_check_json_uint(json.data(),json.size(),"value",&number)==1 && number==4294967295u);
    need(update_check_json_value(json.data(),json.size(),"object",&value,&length)==1 && std::string(value,length)==R"({"a":["b"]})");
    for(const auto* malformed:{R"({"v":4294967296})",R"({"v":-1})",R"({"v":01})",R"({"v":1.0})",R"({"v":null})",R"({"v":0,"v":1})",R"({"v":1,})"}) {
        const std::string item=malformed;need(update_check_json_uint(item.data(),item.size(),"v",&number)==-1);
    }
    json=R"(["one","two\u002ev1"])";
    need(update_check_json_array_string(json.data(),json.size(),1,text,sizeof(text))==1 && std::string{text}=="two.v1");
    need(update_check_json_array_string(json.data(),json.size(),2,text,sizeof(text))==0);
    for(const auto* malformed:{R"(["one",])",R"([true])",R"(["one"] trailing)",R"(["unterminated])",R"(["one\u0000hidden"])"}) {
        const std::string item=malformed;need(update_check_json_array_string(item.data(),item.size(),0,text,sizeof(text))==-1);
    }
    // The wait for the app to close before its files are replaced (wait_for_close): the sandbox
    // going, the app's connection ending with the sandbox left behind (Swordpdf/ProsperoEden#232),
    // a timeout with the connection open, a cancel, and an unreadable sandbox folder.
    {
        const fs::path sandboxes=root/"sandbox";
        self_update::Environment environment;
        environment.sandboxes=sandboxes.string();
        unsigned now=0;std::function<void(unsigned)> at;
        environment.sleep=[&](unsigned ms){now+=ms;if(at) at(now);};
        const std::string title="PPSA64448";
        bool gone=false,stop=false;
        auto wait=[&]{return self_update::wait_for_close(environment,title,[&]{return gone;},[&]{return stop;});};
        auto reset=[&]{now=0;at=nullptr;gone=stop=false;fs::remove_all(sandboxes);fs::create_directories(sandboxes);};
        reset();need(wait()==self_update::CloseWait::closed && now==0);
        reset();fs::create_directories(sandboxes/"PPSA64448_000");fs::create_directories(sandboxes/"PPSA99001_000");
        at=[&](unsigned t){if(t>=3000) fs::remove_all(sandboxes/"PPSA64448_000");};
        need(wait()==self_update::CloseWait::closed && now==3000);
        reset();fs::create_directories(sandboxes/"PPSA64448_000");
        need(wait()==self_update::CloseWait::timed_out && now>=environment.exit_wait_ms);
        reset();fs::create_directories(sandboxes/"PPSA64448_000");
        at=[&](unsigned t){if(t>=2000) gone=true;};
        need(wait()==self_update::CloseWait::left_behind && now>=2000+environment.stale_sandbox_ms && now<2000+environment.stale_sandbox_ms+1000);
        reset();fs::create_directories(sandboxes/"PPSA64448_000");
        at=[&](unsigned t){if(t>=200000) gone=true;};  // the connection ends after the timeout: never seen
        need(wait()==self_update::CloseWait::timed_out && now<200000);
        reset();fs::create_directories(sandboxes/"PPSA64448_000");
        at=[&](unsigned t){if(t>=1000) stop=true;};
        need(wait()==self_update::CloseWait::cancelled);
        reset();fs::remove_all(sandboxes);
        need(wait()==self_update::CloseWait::timed_out);  // unknown counts as running...
        reset();fs::remove_all(sandboxes);gone=true;
        need(wait()==self_update::CloseWait::left_behind);  // ...until the app's connection ended
    }
    release_layout(root/"release",runtime,checks);
    std::cout<<"PASS: "<<checks<<" application-update compatibility, bounded JSON and release ZIP checks\n";
}
