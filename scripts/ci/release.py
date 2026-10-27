#!/usr/bin/env python3
"""Publish Beta 2 only from the successful master merge of the RSX-26 PR."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[2]


def eligible(event, ref, sha, repository, pull_requests):
    if event != 'push' or ref != 'refs/heads/master':
        return False
    return any(pr.get('number') == 8 and pr.get('merged_at') and
               pr.get('merge_commit_sha') == sha and
               pr.get('base', {}).get('ref') == 'master' and
               pr.get('head', {}).get('ref') == 'RSX-26' and
               (pr.get('head', {}).get('repo') or {}).get('full_name') == repository
               for pr in pull_requests)


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


def context():
    repository, sha = os.environ['GITHUB_REPOSITORY'], os.environ['GITHUB_SHA']
    prs = []
    if os.environ['GITHUB_EVENT_NAME'] == 'push' and os.environ['GITHUB_REF'] == 'refs/heads/master':
        pr = api(f'repos/{repository}/pulls/8', missing_ok=True)
        if pr:
            prs.append(pr)
            merge = pr.get('merge_commit_sha')
            if (merge and merge != sha and eligible('push', 'refs/heads/master', merge, repository, [pr])):
                comparison = api(f'repos/{repository}/compare/{merge}...{sha}')
                if comparison.get('status') == 'ahead':
                    existing = api(f'repos/{repository}/releases/tags/v0.2.0-beta.2', missing_ok=True)
                    if not existing or (existing.get('draft') and existing.get('target_commitish') == sha):
                        # Follow-up fixes retain the owner's approved release intent.
                        prs = [dict(pr, merge_commit_sha=sha)]
    return repository, sha, eligible(os.environ['GITHUB_EVENT_NAME'], os.environ['GITHUB_REF'], sha, repository, prs)


def validated_run_context(run_id):
    if os.environ['GITHUB_EVENT_NAME'] != 'workflow_dispatch' or os.environ['GITHUB_REF'] != 'refs/heads/master':
        raise RuntimeError('Release recovery must run manually from master')
    if not str(run_id).isdecimal():
        raise RuntimeError('A numeric validated workflow run ID is required')
    repository = os.environ['GITHUB_REPOSITORY']
    run = api(f'repos/{repository}/actions/runs/{run_id}')
    if (run['status'] != 'completed' or run['conclusion'] != 'success' or
            run['head_branch'] != 'master' or run['event'] != 'push' or run['path'] != '.github/workflows/release.yml'):
        raise RuntimeError('Recovery requires a successful master release build')
    required = {'Release gate', 'Release readiness', 'Ubuntu x86-64 DEB',
                'Windows x86-64 portable ZIP', 'macOS x86_64', 'macOS arm64', 'Universal macOS DMG'}
    jobs = api(f'repos/{repository}/actions/runs/{run_id}/jobs?per_page=100')['jobs']
    passed = {job['name'] for job in jobs if job['conclusion'] == 'success'}
    if not required <= passed:
        raise RuntimeError('Recovery build is missing successful required checks: ' + str(required - passed))
    pr = api(f'repos/{repository}/pulls/8')
    merge, sha = pr.get('merge_commit_sha'), run['head_sha']
    if not merge or not eligible('push', 'refs/heads/master', merge, repository, [pr]):
        raise RuntimeError('The release PR has not been approved and merged')
    if sha != merge and api(f'repos/{repository}/compare/{merge}...{sha}')['status'] != 'ahead':
        raise RuntimeError('The validated build does not contain the approved release merge')
    return repository, sha, True


def verify_assets(directory, version):
    names = [f'openxmb_{version.replace("-beta.", "~beta.")}_amd64.deb',
             f'OpenXMB-{version}-windows-x86_64.zip', f'OpenXMB-{version}-macos-universal.dmg']
    lines = []
    for name in names:
        path = directory / name
        if not path.is_file() or path.stat().st_size < 1024:
            raise RuntimeError('Missing release artifact: ' + name)
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        recorded = path.with_name(name + '.sha256').read_text().split()[0]
        if actual != recorded:
            raise RuntimeError('Artifact checksum mismatch: ' + name)
        lines.append(actual + '  ' + name)
    checksum = directory / 'SHA256SUMS'
    checksum.write_text('\n'.join(lines) + '\n')
    return [directory / name for name in names] + [checksum]


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
    parser.add_argument('command', choices=['gate', 'publish', 'recover'])
    parser.add_argument('--run-id')
    parser.add_argument('--artifacts', type=Path, default=Path('dist/release'))
    args = parser.parse_args()
    version = (ROOT / 'VERSION').read_text().strip()
    if version != '0.2.0-beta.2' or not re.fullmatch(r'\d+\.\d+\.\d+-beta\.\d+', version):
        raise RuntimeError('This release gate is reserved for OpenXMB 0.2.0 Beta 2')
    repository, sha, publish = validated_run_context(args.run_id) if args.command == 'recover' else context()
    if args.command == 'gate':
        if publish:
            # Reserve the approved commit while it is the master tip. Publication
            # still waits for all builds, packaged-app checks and readiness.
            reserve_tag(repository, sha, 'v' + version)
        with Path(os.environ['GITHUB_OUTPUT']).open('a') as handle:
            handle.write(f'version={version}\npublish={str(publish).lower()}\n')
        print('Publication eligible: ' + str(publish))
        return
    if not publish:
        raise RuntimeError('Publishing requires the master merge of RSX-26 PR #8')
    assets = verify_assets(args.artifacts, version)
    tag = 'v' + version
    notes = ROOT / 'docs/releases' / (tag + '.md')
    release = find_release(repository, tag)
    if release and release['target_commitish'] != sha:
        raise RuntimeError('The release already targets a different commit')
    if release and not release['draft']:
        print('The verified master release has already been published; nothing changed.')
        return
    if not release:
        # Reserve the explicit tag first. Release creation must not implicitly
        # create a tag against an older commit using the integration token.
        reserve_tag(repository, sha, tag)
        subprocess.run(['gh', 'release', 'create', tag, '--repo', repository,
                        '--target', sha, '--verify-tag', '--draft', '--prerelease', '--title', 'OpenXMB 0.2.0 Beta 2',
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
