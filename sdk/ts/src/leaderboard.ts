/**
 * Leaderboard resource (`/v1/leaderboard`).
 *
 * Read-only; available to any authenticated submitter. Returns the
 * current top-N entries with composite/latency/throughput scores
 * already weighted by the active scoring profile.
 */

import type { VelocityClient } from './client.js'
import type { Profile } from './benchmarks.js'

export type LeaderboardEntry = {
  rank: number
  submission_id: string
  team: string
  display: string
  composite_score: number
  latency_score: number
  throughput_score: number
  profile: Profile
  updated_at_ns: number
}

export class LeaderboardResource {
  constructor(private readonly client: VelocityClient) {}

  async top(limit = 50): Promise<LeaderboardEntry[]> {
    const resp = await this.client.request<{ entries: LeaderboardEntry[] }>(
      'GET', `/v1/leaderboard?limit=${limit}`,
    )
    return resp.entries
  }
}
