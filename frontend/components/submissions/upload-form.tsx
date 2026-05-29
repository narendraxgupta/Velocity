/**
 * UploadForm — submitter-facing artefact upload.
 *
 * Posts the artefact to `POST /v1/submissions?team=&display=&kind=&filename=`
 * with the file's bytes as the raw body. The response carries the assigned
 * submission_id, which we use to redirect to the detail page.
 */

'use client'

import { useRouter } from 'next/navigation'
import { useState, type FormEvent } from 'react'
import { toast } from 'sonner'

import { Panel, PanelHeader, PanelTitle, PanelDescription } from '@/components/ui/panel'
import { apiFetch } from '@/lib/api/client'

type UploadResult =
  | { kind: 'ok'; submissionId: string; sha256: string }
  | { kind: 'error'; message: string }

export function UploadForm() {
  const router = useRouter()
  const [team, setTeam] = useState('')
  const [display, setDisplay] = useState('')
  const [kind, setKind] = useState<
    'DOCKERFILE' | 'SOURCE_TAR' | 'BINARY' | 'OCI_IMAGE' | 'PCAP_REPLAY'
  >('DOCKERFILE')
  const [file, setFile] = useState<File | null>(null)
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<UploadResult | null>(null)

  const onSubmit = async (e: FormEvent) => {
    e.preventDefault()
    if (!file) return
    setBusy(true)
    setResult(null)
    try {
      const qs = new URLSearchParams({ team, display, kind, filename: file.name })
      const resp = await apiFetch(`/v1/submissions?${qs.toString()}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: file,
      })
      if (!resp.ok) {
        const detail = await resp.text()
        const message = detail || `HTTP ${resp.status}`
        setResult({ kind: 'error', message })
        toast.error('Upload rejected', { description: message })
        return
      }
      const body = await resp.json()
      setResult({
        kind: 'ok',
        submissionId: body.submission_id,
        sha256: body.sha256,
      })
      toast.success('Submission registered', {
        description: `${body.submission_id} — opening detail view`,
        action: {
          label: 'View',
          onClick: () =>
            router.push(`/submissions/${encodeURIComponent(body.submission_id)}`),
        },
      })
    } catch (err) {
      const message = (err as Error).message
      setResult({ kind: 'error', message })
      toast.error('Upload failed', { description: message })
    } finally {
      setBusy(false)
    }
  }

  return (
    <Panel>
      <PanelHeader>
        <PanelTitle>Submit an artefact</PanelTitle>
        <PanelDescription>
          The platform accepts a Dockerfile context tar, a source tarball, a static binary, a
          pre-built OCI image, or a recorded <code className="font-mono">.pcap</code> for
          deterministic byte-stream replay. SHA-256 is computed server-side as the upload streams
          into MinIO.
        </PanelDescription>
      </PanelHeader>

      <form onSubmit={onSubmit} className="grid grid-cols-1 gap-4 p-4 md:grid-cols-2">
        <Field label="Team">
          <input
            required
            className="form-input"
            value={team}
            onChange={(e) => setTeam(e.target.value)}
            placeholder="velocity-internal"
          />
        </Field>
        <Field label="Display name">
          <input
            required
            className="form-input"
            value={display}
            onChange={(e) => setDisplay(e.target.value)}
            placeholder="matchify-v4"
          />
        </Field>
        <Field label="Artefact kind">
          <select
            className="form-input"
            value={kind}
            onChange={(e) => setKind(e.target.value as typeof kind)}
          >
            <option value="DOCKERFILE">Dockerfile (tar)</option>
            <option value="SOURCE_TAR">Source tarball</option>
            <option value="BINARY">Static binary</option>
            <option value="OCI_IMAGE">Pre-built OCI image (tar)</option>
            <option value="PCAP_REPLAY">.pcap replay (no build)</option>
          </select>
        </Field>
        <Field label="File">
          <input
            required
            type="file"
            className="form-input"
            onChange={(e) => setFile(e.target.files?.[0] ?? null)}
          />
        </Field>

        <div className="md:col-span-2 flex items-center justify-between gap-3 pt-2">
          <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
            {file ? formatBytes(file.size) : 'select a file'}
          </span>
          <button
            type="submit"
            disabled={!file || busy}
            className="btn-primary disabled:cursor-not-allowed disabled:opacity-50"
          >
            {busy ? 'Uploading…' : 'Upload & register'}
          </button>
        </div>

        {kind === 'PCAP_REPLAY' && (
          <div className="md:col-span-2 rounded-md border border-signal-info/40 bg-signal-info/5 p-3 font-mono text-2xs text-foreground">
            <div className="text-signal-info">PCAP replay submission</div>
            <div className="mt-1 text-muted-foreground">
              No container will be built. After upload, go to{' '}
              <code className="font-mono">/admin</code> → <em>Pcap replay</em> and choose a target
              submission to fire this capture at. The replayer preserves original inter-packet
              timing (or you can switch to a fixed RPS).
            </div>
          </div>
        )}

        {result && result.kind === 'ok' && (
          <div className="md:col-span-2 rounded-md border border-signal-live/40 bg-signal-live/5 p-3 font-mono text-2xs text-foreground">
            <div className="text-signal-live">Uploaded</div>
            <div className="mt-1">submission_id: {result.submissionId}</div>
            <div>sha256:       {result.sha256}</div>
          </div>
        )}
        {result && result.kind === 'error' && (
          <div className="md:col-span-2 rounded-md border border-signal-ask/40 bg-signal-ask/5 p-3 font-mono text-2xs text-signal-ask">
            Upload failed: {result.message}
          </div>
        )}
      </form>
    </Panel>
  )
}

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label className="flex flex-col gap-1.5">
      <span className="font-mono text-2xs uppercase tracking-widest text-muted-foreground">
        {label}
      </span>
      {children}
    </label>
  )
}

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MiB`
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GiB`
}
