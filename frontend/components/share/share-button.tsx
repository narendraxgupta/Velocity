/**
 * <ShareButton> — mints a public share URL for the given run.
 *
 * Clicking the button calls POST /v1/share, receives the token, and
 * shows a small popover with the URL + copy-to-clipboard. We also
 * surface the TTL clearly because "permanent share link" is a footgun
 * in a benchmarking context.
 *
 * The button is hidden for users without `submissions:write` — they
 * can't own submissions so minting a share would always 403 at the
 * gateway anyway.
 */

'use client'

import { useState } from 'react'
import { toast } from 'sonner'

import { RoleGate } from '@/components/auth/role-gate'
import { apiJson } from '@/lib/api/client'

type Props = {
  submissionId: string
  benchmarkId:  string
  ttlSeconds?:  number
}

type MintResp = {
  token: string
  url:   string
  ttl_seconds: number
}

export function ShareButton({ submissionId, benchmarkId, ttlSeconds }: Props) {
  return (
    <RoleGate cap="submissions:write">
      <ShareButtonInner
        submissionId={submissionId}
        benchmarkId={benchmarkId}
        ttlSeconds={ttlSeconds}
      />
    </RoleGate>
  )
}

function ShareButtonInner({ submissionId, benchmarkId, ttlSeconds }: Props) {
  const [busy, setBusy] = useState(false)
  const [shareUrl, setShareUrl] = useState<string | null>(null)
  const [ttl, setTtl] = useState<number | null>(null)

  const mint = async () => {
    setBusy(true)
    try {
      const body: Record<string, unknown> = {
        submission_id: submissionId,
        benchmark_id:  benchmarkId,
      }
      if (ttlSeconds) body.ttl_seconds = ttlSeconds
      const data = await apiJson<MintResp>('/v1/share', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      const full = `${window.location.origin}${data.url}`
      setShareUrl(full)
      setTtl(data.ttl_seconds)
      try {
        await navigator.clipboard.writeText(full)
        toast.success('Share URL copied to clipboard')
      } catch {
        // Permissions API denied — still show the URL in the popover.
      }
    } catch (e) {
      toast.error('Failed to mint share', { description: String(e) })
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="inline-flex items-center gap-3">
      <button
        onClick={mint}
        disabled={busy}
        className="rounded-md border border-border bg-surface px-3 py-1.5 font-mono text-2xs font-semibold uppercase tracking-widest text-foreground hover:border-accent hover:text-accent disabled:opacity-40"
      >
        {busy ? 'Minting…' : shareUrl ? 'Re-share' : 'Share publicly'}
      </button>
      {shareUrl && (
        <span className="font-mono text-2xs text-muted-foreground">
          <a href={shareUrl} className="text-accent hover:underline">
            {shareUrl}
          </a>
          {ttl != null && (
            <>
              {' '}· expires in {Math.round(ttl / 86400)}d
            </>
          )}
        </span>
      )}
    </div>
  )
}
