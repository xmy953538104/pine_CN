#!/usr/bin/env python3
"""Candidate gate: exact source, completed successful fast suite. No artifact rebuild by label."""
import json
import os
import subprocess
from pathlib import Path
import urllib.request
import importlib.util

spec = importlib.util.spec_from_file_location('scopes', Path(__file__).with_name('diagnostics-changes.py'))
scopes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scopes)


def trusted(run, repository):
    return (run.get('name') == 'Diagnostics fast checks' and
            run.get('path') == '.github/workflows/diagnostics-fast.yml' and
            run.get('event') in ('push', 'workflow_dispatch') and
            run.get('head_repository', {}).get('full_name') == repository)


def require(runs, commit, repository):
    matching = [run for run in runs if run.get('name') == 'Diagnostics fast checks'
                and run.get('head_sha') == commit
                and trusted(run, repository)]
    latest = max(matching, key=lambda run: run['id'], default={})
    if latest.get('status') != 'completed' or latest.get('conclusion') != 'success':
        raise ValueError(f'Fast checks have not passed for exact source {commit}: {latest.get("html_url", "no run")}')
    return latest['html_url']


def qualify(runs, commit, jobs, differences):
    """Reuse a suite only with an exact unchanged input scope, never by a green badge."""
    evidence = {}
    for run in sorted(runs, key=lambda item: item['id'], reverse=True):
        if run.get('name') != 'Diagnostics fast checks' or run.get('conclusion') != 'success':
            continue
        changes = scopes.classify(differences(run['head_sha'], commit))
        for job in jobs(run['id']):
            domain = job.get('name')
            if domain in changes and domain not in evidence and job.get('conclusion') == 'success' and not changes[domain]:
                evidence[domain] = {'source_commit': run['head_sha'], 'run_id': run['id'], 'job_id': job['id'],
                                    'url': job['html_url'], 'inputs': 'exact unchanged scope' if run['head_sha'] != commit else 'exact source'}
        if len(evidence) == 4:
            return evidence
    raise ValueError('No passing unchanged-scope evidence for: ' + ', '.join(sorted(set(('native', 'tools', 'windows', 'jvm')) - evidence.keys())))


if __name__ == '__main__':
    commit = os.environ['EXPECTED_COMMIT']
    repository = os.environ['GITHUB_REPOSITORY']
    def get(path):
        request = urllib.request.Request(f'https://api.github.com/repos/{repository}/{path}', headers={
            'Authorization': 'Bearer ' + os.environ['GH_TOKEN'], 'Accept': 'application/vnd.github+json'})
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    runs = get('actions/workflows/diagnostics-fast.yml/runs?per_page=50')['workflow_runs']
    # Fork and intentionally skipped same-repository PR wake-ups are not trusted
    # build qualifications. Push/manual runs use the source branch directly.
    runs = [run for run in runs if trusted(run, repository)]
    exact = require(runs, commit, repository)
    def differences(previous, current):
        if previous == current: return []
        subprocess.run(['git', 'fetch', '--no-tags', '--depth=1', 'origin', previous], check=True)
        return scopes.diff_paths(previous, current)
    result = {'source_commit': commit, 'exact_source_fast_run': exact,
              'suite_evidence': qualify(runs, commit, lambda run: get(f'actions/runs/{run}/jobs?per_page=100')['jobs'], differences)}
    Path('fast-qualification.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
