#!/usr/bin/env python3
"""Candidate gate: exact source, completed successful fast suite. No artifact rebuild by label."""
import json
import os
import urllib.request


def require(runs, commit, repository=None):
    matching = [run for run in runs if run.get('name') == 'Diagnostics fast checks'
                and run.get('head_sha') == commit
                and not (repository and run.get('event') == 'pull_request' and
                         run.get('head_repository', {}).get('full_name') == repository)]
    latest = max(matching, key=lambda run: run['id'], default={})
    if latest.get('status') != 'completed' or latest.get('conclusion') != 'success':
        raise ValueError(f'Fast checks have not passed for exact source {commit}: {latest.get("html_url", "no run")}')
    return latest['html_url']


if __name__ == '__main__':
    commit = os.environ['EXPECTED_COMMIT']
    url = f'https://api.github.com/repos/{os.environ["GITHUB_REPOSITORY"]}/actions/runs?head_sha={commit}&per_page=100'
    request = urllib.request.Request(url, headers={
        'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(request, timeout=30) as response:
        runs = json.load(response)['workflow_runs']
    print('Exact-source fast checks passed: ' + require(runs, commit, os.environ['GITHUB_REPOSITORY']))
