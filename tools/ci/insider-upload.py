#!/usr/bin/env python3
# nightly insider: upload a build to the insider R2 bucket and list it in the app's manifest.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The canonical copy is Oliv3rmoon/nightly-insider tools/insider_upload.py; Nightshade and Dulcamara carry the same
# file for their CI (tests: tools/test_insider_upload.py there). Python 3 standard library plus the AWS CLI, which
# GitHub's ubuntu runners have; R2 speaks the S3 API.
#
#   insider_upload.py --app nightshade --name Nightshade --tag v1.010.013 \
#       --zip dist/Nightshade-v1.010.013.zip --sums dist/SHA256SUMS --notes-file dist/release-notes.md --same-line
#
# Environment: R2_ACCOUNT_ID, R2_BUCKET, AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY (an R2 API token's S3 keys,
# Object Read & Write on that bucket only). INSIDER_WINDOW_DAYS (default 7): when the build is meant to go public.
#
# What it does, in order, stopping at the first problem (nothing in the manifest changes until the files are up):
#   1. checks the ZIP against SHA256SUMS (exactly one well-formed line for it);
#   2. reads manifests/<app>.json (none yet: an empty one). A build of this version already listed with the same
#      SHA-256 is a re-run: nothing to do. With another SHA-256 it is refused: an insider build, like a GitHub
#      release ZIP, never changes once members may have checked it;
#   3. uploads builds/<app>/<tag>/<zip> and builds/<app>/<tag>/SHA256SUMS, and checks the ZIP's size there;
#   4. writes the manifest with the build added, newest first, at most --keep builds;
#   5. deletes the files of builds that fell off the manifest, so the bucket holds --keep builds an app at most.

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

APP = re.compile(r"^[a-z][a-z0-9-]{1,31}$")
TAG = re.compile(r"^v?(\d{1,6})\.(\d{1,6})\.(\d{1,6})$")
FILE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$")
SUMS_LINE = re.compile(r"^([0-9a-fA-F]{64}) [ *](.+)$")
NOTES_MAX = 4000


class UploadError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sums_for(sums_text, name):
    found = []
    for line in sums_text.splitlines():
        line = line.rstrip("\r")
        if not line.strip():
            continue
        match = SUMS_LINE.match(line)
        if match and match.group(2) == name:
            found.append(match.group(1).lower())
        elif not match and name in line:
            raise UploadError(f"SHA256SUMS has a malformed line for {name}")
    if len(found) != 1:
        raise UploadError(f"SHA256SUMS has {len(found)} lines for {name}, not one")
    return found[0]


def plain_notes(markdown):
    """Release notes as plain text the apps show as they are: headings, emphasis, code marks and link addresses go."""
    out = []
    for line in markdown.replace("\r\n", "\n").split("\n"):
        line = re.sub(r"^\s{0,3}#{1,6}\s*", "", line)
        line = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", line)
        line = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", line)
        line = re.sub(r"(\*\*|__|`)", "", line)
        line = re.sub(r"^\s*[*+]\s+", "- ", line)
        line = re.sub(r"<[^>]{1,200}>", "", line)
        out.append(line.rstrip())
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    if len(text) > NOTES_MAX:
        cut = text.rfind("\n", 0, NOTES_MAX - 2)
        text = text[: cut if cut > 0 else NOTES_MAX - 2].rstrip() + "\n…"
    return text


def version_key(version):
    return tuple(int(p) for p in TAG.match(version).groups())


class Bucket:
    def __init__(self, aws, endpoint, bucket):
        self.aws, self.endpoint, self.bucket = aws, endpoint, bucket
        self.env = dict(os.environ)
        self.env.update(
            {
                "AWS_DEFAULT_REGION": "auto",
                "AWS_EC2_METADATA_DISABLED": "true",
                # R2 doesn't need the newer default request checksums of the AWS CLI.
                "AWS_REQUEST_CHECKSUM_CALCULATION": "WHEN_REQUIRED",
                "AWS_RESPONSE_CHECKSUM_VALIDATION": "WHEN_REQUIRED",
            }
        )

    def run(self, *args):
        command = [*self.aws, *args, "--endpoint-url", self.endpoint]
        return subprocess.run(command, env=self.env, capture_output=True, text=True)

    def get(self, key, dest):
        """True when the object was downloaded; False when there is none."""
        result = self.run("s3api", "get-object", "--bucket", self.bucket, "--key", key, dest)
        if result.returncode == 0:
            return True
        if re.search(r"NoSuchKey|Not Found|\(404\)", result.stderr):
            return False
        raise UploadError(f"reading {key} failed: {result.stderr.strip()[:300]}")

    def put(self, path, key, content_type, cache_control=None):
        args = ["s3", "cp", path, f"s3://{self.bucket}/{key}", "--no-progress", "--content-type", content_type]
        if cache_control:
            args += ["--cache-control", cache_control]
        result = self.run(*args)
        if result.returncode != 0:
            raise UploadError(f"uploading {key} failed: {result.stderr.strip()[:300]}")

    def remove_prefix(self, prefix):
        result = self.run("s3", "rm", f"s3://{self.bucket}/{prefix}", "--recursive")
        if result.returncode != 0:
            # The manifest no longer lists them, so nothing offers them; the files only take space.
            print(f"insider upload: warning: removing {prefix} failed: {result.stderr.strip()[:300]}", file=sys.stderr)

    def exists(self, key):
        result = self.run("s3api", "head-object", "--bucket", self.bucket, "--key", key)
        if result.returncode == 0:
            return True
        if re.search(r"NoSuchKey|Not Found|\(404\)", result.stderr):
            return False
        raise UploadError(f"checking {key} failed: {result.stderr.strip()[:300]}")

    def size(self, key):
        result = self.run("s3api", "head-object", "--bucket", self.bucket, "--key", key)
        if result.returncode != 0:
            raise UploadError(f"checking {key} failed: {result.stderr.strip()[:300]}")
        return int(json.loads(result.stdout)["ContentLength"])


def upload(args, bucket, now=None):
    if not APP.match(args.app):
        raise UploadError(f"app {args.app!r} is not a valid id")
    tag_match = TAG.match(args.tag)
    if not tag_match:
        raise UploadError(f"tag {args.tag!r} is not a version")
    version = ".".join(tag_match.groups()) if not args.version else args.version
    if version_key(version) != version_key(args.tag):
        raise UploadError(f"version {version} is not the tag's {args.tag}")
    file = os.path.basename(args.zip)
    if not FILE.match(file):
        raise UploadError(f"file name {file!r} can't be served")

    sha256 = sha256_file(args.zip)
    with open(args.sums, encoding="utf-8") as f:
        listed = sums_for(f.read(), file)
    if listed != sha256:
        raise UploadError(f"{file} is {sha256}, but SHA256SUMS says {listed}")
    size = os.path.getsize(args.zip)

    prefix = f"builds/{args.app}/{args.tag}"
    key = f"{prefix}/{file}"
    manifest_key = f"manifests/{args.app}.json"
    with tempfile.TemporaryDirectory() as tmp:
        current = os.path.join(tmp, "manifest.json")
        if bucket.get(manifest_key, current):
            with open(current, encoding="utf-8") as f:
                manifest = json.load(f)
            if manifest.get("app") != args.app or not isinstance(manifest.get("builds"), list):
                raise UploadError(f"{manifest_key} is not {args.app}'s manifest")
        else:
            manifest = {"app": args.app, "builds": []}

        for build in manifest["builds"]:
            if TAG.match(str(build.get("version", ""))) and version_key(build["version"]) == version_key(version):
                if build.get("sha256") == sha256 and build.get("key") == key:
                    print(f"{args.app} {args.tag} is already an insider build with this SHA-256: nothing to do")
                    return manifest
                raise UploadError(
                    f"{args.app} {version} is already an insider build with another file or SHA-256; "
                    "insider builds never change: tag a new version"
                )

        # The files may outlive their manifest entry (a build pulled by hand): never put other bytes under them.
        stored_sums = os.path.join(tmp, "stored-SHA256SUMS")
        if bucket.exists(key):
            if not bucket.get(f"{prefix}/SHA256SUMS", stored_sums):
                raise UploadError(f"{key} is already in the bucket without its SHA256SUMS; tag a new version")
            with open(stored_sums, encoding="utf-8") as f:
                if sums_for(f.read(), file) != sha256:
                    raise UploadError(f"{key} is already in the bucket with another SHA-256; insider builds never change: tag a new version")

        bucket.put(args.zip, key, "application/zip")
        bucket.put(args.sums, f"{prefix}/SHA256SUMS", "text/plain; charset=utf-8")
        if bucket.size(key) != size:
            raise UploadError(f"{key} in R2 is not {size} bytes")

        notes = ""
        if args.notes_file and os.path.exists(args.notes_file):
            with open(args.notes_file, encoding="utf-8") as f:
                notes = plain_notes(f.read())
        now = now or datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
        window = datetime.timedelta(days=args.window_days)
        stamp = lambda t: t.isoformat().replace("+00:00", "Z")
        entry = {
            "version": version,
            "tag": args.tag,
            "title": args.title or f"{args.name} {args.tag} (insider build)",
            "file": file,
            "key": key,
            "sums_key": f"{prefix}/SHA256SUMS",
            "size": size,
            "sha256": sha256,
            "notes": notes,
            "published_at": stamp(now),
            "public_after": stamp(now + window),
        }
        builds = [b for b in manifest["builds"] if TAG.match(str(b.get("version", "")))] + [entry]
        builds.sort(key=lambda b: version_key(b["version"]), reverse=True)
        kept, dropped = builds[: args.keep], builds[args.keep :]
        manifest = {"app": args.app, "name": args.name, "same_line": bool(args.same_line), "builds": kept}
        out = os.path.join(tmp, "new-manifest.json")
        with open(out, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, ensure_ascii=False)
            f.write("\n")
        bucket.put(out, manifest_key, "application/json; charset=utf-8", "no-store")
        # A dropped build's folder is builds/<app>/<tag>/, taken from its key and checked: never a folder that holds
        # a build still listed.
        kept_folders = {str(b.get("key", "")).rsplit("/", 1)[0] + "/" for b in kept}
        for old in dropped:
            match = re.fullmatch(rf"(builds/{re.escape(args.app)}/(v?\d{{1,6}}\.\d{{1,6}}\.\d{{1,6}})/)[A-Za-z0-9][A-Za-z0-9._-]{{0,99}}", str(old.get("key", "")))
            if match and match.group(1) not in kept_folders:
                bucket.remove_prefix(match.group(1))
    print(f"{args.app} {args.tag}: {file} ({size} bytes, sha256 {sha256}) is an insider build; public after {entry['public_after']}")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description="Upload an insider build to R2 and list it in the app's manifest.")
    parser.add_argument("--app", required=True, help="the app's id: nightshade, dulcamara")
    parser.add_argument("--name", required=True, help="the app's name: Nightshade, Dulcamara")
    parser.add_argument("--tag", required=True, help="the release tag: v1.010.013, 0.2.14")
    parser.add_argument("--version", help="the version, when it isn't the tag's numbers")
    parser.add_argument("--zip", required=True)
    parser.add_argument("--sums", required=True, help="SHA256SUMS listing the ZIP")
    parser.add_argument("--notes-file", help="Markdown release notes (made plain text, at most 4000 characters)")
    parser.add_argument("--title")
    parser.add_argument("--same-line", action="store_true", help="the app offers builds of its own version line only")
    parser.add_argument("--keep", type=int, default=5, help="builds the manifest lists, newest first; older ones are deleted")
    parser.add_argument("--window-days", type=float, default=float(os.environ.get("INSIDER_WINDOW_DAYS") or 7))
    args = parser.parse_args(argv)
    if args.keep < 1:
        parser.error("--keep must be at least 1")

    account = os.environ.get("R2_ACCOUNT_ID", "").strip()
    bucket_name = os.environ.get("R2_BUCKET", "").strip()
    if not re.fullmatch(r"[0-9a-f]{32}", account) or not bucket_name:
        print("insider upload: R2_ACCOUNT_ID (32 hex) and R2_BUCKET must be set", file=sys.stderr)
        return 2
    if not os.environ.get("AWS_ACCESS_KEY_ID") or not os.environ.get("AWS_SECRET_ACCESS_KEY"):
        print("insider upload: the R2 keys (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY) are not set", file=sys.stderr)
        return 2
    aws = os.environ.get("INSIDER_AWS", "aws").split()
    bucket = Bucket(aws, f"https://{account}.r2.cloudflarestorage.com", bucket_name)
    try:
        upload(args, bucket)
    except UploadError as e:
        print(f"insider upload: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
