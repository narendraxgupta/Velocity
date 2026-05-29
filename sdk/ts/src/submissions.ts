import type { VelocityClient } from './client.js'

export type SubmissionKind = 'matching_engine' | 'market_maker' | 'pcap_replay'

export type Submission = {
  id: string
  team: string
  display: string
  kind: SubmissionKind
  status: string
  created_at_ns: number
}

export type CreateSubmissionRequest = {
  team: string
  display: string
  kind: SubmissionKind
  source?: Blob | ArrayBuffer | Uint8Array
}

export class SubmissionsResource {
  constructor(private readonly client: VelocityClient) {}

  async create(req: CreateSubmissionRequest): Promise<Submission> {
    const resp = await this.client.request<{ submission: Submission; upload_url?: string }>(
      'POST', '/v1/submissions',
      { team: req.team, display: req.display, kind: req.kind },
    )
    if (req.source && resp.upload_url) {
      const body =
        req.source instanceof Blob ? req.source :
        // Copy into a fresh ArrayBuffer-backed view: a bare `Uint8Array`
        // is `Uint8Array<ArrayBufferLike>` (TS 5.7+), which `BlobPart`
        // rejects because the backing buffer could be a SharedArrayBuffer.
        req.source instanceof Uint8Array ? new Blob([new Uint8Array(req.source)]) :
        new Blob([req.source])
      // Direct PUT to the signed URL — no auth header because the
      // signature is in the URL query params.
      const res = await fetch(resp.upload_url, { method: 'PUT', body })
      if (!res.ok) throw new Error(`source upload failed: HTTP ${res.status}`)
      await this.client.request<void>('POST',
        `/v1/submissions/${encodeURIComponent(resp.submission.id)}/uploaded`,
        { id: resp.submission.id },
      )
    }
    return resp.submission
  }

  async get(id: string): Promise<Submission> {
    const resp = await this.client.request<{ submission: Submission }>(
      'GET', `/v1/submissions/${encodeURIComponent(id)}`,
    )
    return resp.submission
  }

  async list(limit = 20): Promise<Submission[]> {
    const resp = await this.client.request<{ submissions: Submission[] }>(
      'GET', `/v1/submissions?limit=${limit}`,
    )
    return resp.submissions
  }
}
