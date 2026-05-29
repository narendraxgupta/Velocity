/**
 * Submissions page — upload + lifecycle view.
 */

import type { Metadata } from 'next'

import { UploadForm } from '@/components/submissions/upload-form'

export const metadata: Metadata = {
  title: 'Submissions',
}

export default function SubmissionsPage() {
  return (
    <div className="container space-y-6 py-8">
      <header>
        <span className="label-eyebrow">Submission artefacts</span>
        <h1 className="font-display text-2xl font-semibold tracking-tight">Submissions</h1>
        <p className="text-sm text-muted-foreground">
          Upload a matching engine. We containerize it with Kaniko, sandbox it with gVisor,
          and queue it for benchmarking.
        </p>
      </header>

      <UploadForm />
    </div>
  )
}
