#!/usr/bin/env python3
"""Publish an explicitly approved and validated master release."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def api(path, missing_ok=False):
    request = urllib.request.Request('https://api.github.com/' + path, headers={
        'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
        'Accept': 'application/vnd.github+json', 'X-GitHub-Api-Version': '2022-11-28',
    })
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.load(response)
    except urllib.error.HTTPError as error:
        if error.code == 404 and missing_ok:
            return None
        raise


def validated_run_context(run_id, approved_sha):
    if os.environ['GITHUB_EVENT_NAME'] != 'workflow_dispatch' or os.environ['GITHUB_REF'] != 'refs/heads/master':
        raise RuntimeError('Release recovery must run manually from master')
    if not str(run_id).isdecimal():
        raise RuntimeError('A numeric validated workflow run ID is required')
    if not re.fullmatch(r'[0-9a-f]{40}', approved_sha or ''):
        raise RuntimeError('The approved master SHA must be a full lowercase commit SHA')
    if os.environ['GITHUB_SHA'] != approved_sha:
        raise RuntimeError('The publication workflow must run at the approved SHA')
    repository = os.environ['GITHUB_REPOSITORY']
    run = api(f'repos/{repository}/actions/runs/{run_id}')
    if (run['status'] != 'completed' or run['conclusion'] != 'success' or
            run['head_branch'] != 'master' or run['event'] != 'push' or run['path'] != '.github/workflows/release.yml'):
        raise RuntimeError('Recovery requires a successful master release build')
    if run['head_sha'] != approved_sha:
        raise RuntimeError('The validated build does not match the approved SHA')
    required = {'Release gate', 'Release readiness', 'Ubuntu x86-64 DEB',
                'Windows x86-64 portable ZIP', 'macOS x86_64', 'macOS arm64', 'Universal macOS DMG',
                'Portable Linux (ubuntu)', 'Portable Linux (fedora)', 'Portable Linux (arch)'}
    jobs = api(f'repos/{repository}/actions/runs/{run_id}/jobs?per_page=100')['jobs']
    passed = {job['name'] for job in jobs if job['conclusion'] == 'success'}
    if not required <= passed:
        raise RuntimeError('Recovery build is missing successful required checks: ' + str(required - passed))
    if api(f'repos/{repository}/branches/master')['commit']['sha'] != approved_sha:
        raise RuntimeError('Master no longer points to the approved SHA')
    return repository, approved_sha, True


def verify_assets(directory, version):
    names = [f'openxmb_{version.replace("-beta.", "~beta.")}_amd64.deb',
             f'OpenXMB-{version}-windows-x86_64.zip', f'OpenXMB-{version}-macos-universal.dmg',
             f'OpenXMB-{version}-linux-x86_64.tar.gz']
    lines, assets = [], []
    for name in names:
        path = directory / name
        if not path.is_file() or path.stat().st_size < 1024:
            raise RuntimeError('Missing release artifact: ' + name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        recorded = path.with_name(name + '.sha256').read_text().split()[0]
        if actual != recorded:
            raise RuntimeError('Artifact checksum mismatch: ' + name)
        # GitHub normalizes '~' in uploaded asset names to '.'. Keep the Debian
        # package version intact, but publish a stable filename and manifest.
        asset = directory / name.replace('~', '.')
        if asset != path:
            shutil.copyfile(path, asset)
        assets.append(asset)
        lines.append(actual + '  ' + asset.name)
    checksum = directory / 'SHA256SUMS'
    checksum.write_text('\n'.join(lines) + '\n')
    return assets + [checksum]


def reserve_tag(repository, sha, tag):
    tag_ref = api(f'repos/{repository}/git/ref/tags/{tag}', missing_ok=True)
    if not tag_ref:
        subprocess.run(['gh', 'api', '--method', 'POST', f'repos/{repository}/git/refs',
                        '-f', f'ref=refs/tags/{tag}', '-f', f'sha={sha}'], check=True)
    elif tag_ref['object']['sha'] != sha:
        raise RuntimeError('The release tag targets a different commit')


def find_release(repository, tag):
    release = api(f'repos/{repository}/releases/tags/{tag}', missing_ok=True)
    if release:
        return release
    # The by-tag endpoint can omit unpublished drafts; the authenticated list
    # exposes drafts to publishers and avoids attempting to create a duplicate.
    return next((item for item in api(f'repos/{repository}/releases?per_page=100')
                 if item['tag_name'] == tag), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['gate', 'recover'])
    parser.add_argument('--run-id')
    parser.add_argument('--approved-sha')
    parser.add_argument('--artifacts', type=Path, default=Path('dist/release'))
    args = parser.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'\d+\.\d+\.\d+-beta\.\d+', version):
        raise RuntimeError('Expected a beta release version')
    if args.command == 'gate':
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as handle:
            handle.write(f'version={version}\n')
        print('Build validation does not publish a release')
        return
    repository, sha, _ = validated_run_context(args.run_id, args.approved_sha)
    assets = verify_assets(args.artifacts, version)
    tag = 'v' + version
    notes = ROOT / 'docs/releases' / (tag + '.md')
    release = find_release(repository, tag)
    if release and release['target_commitish'] != sha:
        raise RuntimeError('The release already targets a different commit')
    if release and not release['draft']:
        print('The verified master release has already been published; nothing changed.')
        return
    # Check the tag even for existing drafts so recovery cannot upload to a
    # tag that was moved or created for another commit.
    reserve_tag(repository, sha, tag)
    if not release:
        # Reserve the explicit tag first. Release creation must not implicitly
        # create a tag against an older commit using the integration token.
        subprocess.run(['gh', 'release', 'create', tag, '--repo', repository,
                        '--target', sha, '--verify-tag', '--draft', '--prerelease', '--title', 'OpenXMB ' + version,
                        '--notes-file', str(notes), *map(str, assets)], check=True)
    else:
        subprocess.run(['gh', 'release', 'upload', tag, '--repo', repository, '--clobber', *map(str, assets)], check=True)
    release = find_release(repository, tag)
    if {asset['name'] for asset in release['assets']} != {path.name for path in assets}:
        raise RuntimeError('The draft release does not contain exactly the verified release assets')
    # Publishing is deliberately the final mutation, after every upload succeeds.
    subprocess.run(['gh', 'release', 'edit', tag, '--repo', repository,
                    '--draft=false', '--prerelease', '--notes-file', str(notes)], check=True)


if __name__ == '__main__':
    main()
