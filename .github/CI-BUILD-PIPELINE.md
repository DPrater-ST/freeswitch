# RingRx FreeSWITCH — `.deb` build pipeline

`.github/workflows/build-freeswitch-deb.yml` builds this fork's FreeSWITCH Debian
packages and publishes them to S3, where the **telephony-iac** VoiceFabric AMI
pipeline consumes them.

```
build-freeswitch-deb (this repo)         telephony-iac
  debian/bootstrap.sh -c <codename>        packer/freeswitch (build-vf-freeswitch-ami)
  mk-build-deps -i                  →  S3  →  -var fs_artifact_s3_path=<tarball key>
  dpkg-buildpackage -b              artifact   -var artifact_s3_bucket=<bucket>
  → *.deb + SHA256SUMS + tarball              → bakes AMI → SSM /rtc/freeswitch/ami-id
  → s3://<bucket>/<prefix>/st-build-<N>/      → vm-tier/freeswitch ASG
```

## Run
Actions → **build-freeswitch-deb** → Run workflow. Inputs:
- `ref` — the ST RingRx release ref to build (branch/tag/SHA). **The patch branches
  aren't consolidated on `master` yet** — point this at the agreed release ref.
- `st_build` — build number (e.g. `42` → `st-build-42` tag + artifact path).
- `ubuntu_codename` — `jammy` (match the AMI base in telephony-iac).
- `s3_bucket` / `s3_prefix` — the VF artifacts bucket (`freeswitch/ringrx` prefix).

Output (step summary): the `fs_artifact_s3_path` to pass to the telephony-iac
`build-vf-freeswitch-ami` workflow.

## Prereqs (one-time)
1. **Artifacts S3 bucket** — TF-managed in telephony-iac (follow-up; the FS role +
   packer read `artifact_s3_bucket`).
2. **OIDC role** — an AWS IAM role with a federated trust for
   `repo:DPrater-ST/freeswitch:environment:artifacts`, permissions scoped to
   `s3:PutObject`/`s3:GetObject` on `arn:aws:s3:::<bucket>/<prefix>/*`. Put its ARN
   in this repo's **`artifacts`** Environment as secret `AWS_ARTIFACTS_ROLE_ARN`.

## ⚠️ Governance
This is a **public, personal-account** fork. The OIDC role is environment-scoped +
the workflow is dispatch-only, but a production build source should live in the org
(`servicetitan/freeswitch`, currently an empty placeholder). Plan to mirror this
fork there and move the pipeline. The build itself is fiddly (see the
`deb11_compile_issues` branch) — expect to tune deps/modules per codename.
