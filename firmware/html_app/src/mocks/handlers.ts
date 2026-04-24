import { http, HttpResponse } from 'msw'

export type Settings = {
  name: string
}

let settings: Settings = { name: 'Home Energy Manager' }

export const handlers = [
  http.get('/api/settings', () => {
    return HttpResponse.json(settings)
  }),

  http.put('/api/settings', async ({ request }) => {
    const body = (await request.json()) as Settings
    settings = { ...settings, ...body }
    return HttpResponse.json(settings)
  }),
]
