// Shared helpers for the Commodity Tariff data served by /api/tariff.

export type TariffDay = {
  date: string
  assigned: boolean
  provider?: string
  label?: string
  currency?: number // ISO 4217 numeric
  decimals?: number // slot value / 10^decimals = price per kWh in currency units
  unit?: number     // 0 = kWh, 1 = kVAh
  slots: (number | null)[] // 96 x 15-minute prices; empty when the day has no tariff
}

export const SLOT_MINUTES = 15

// Matter carries currency as the ISO 4217 numeric code; Intl needs the alpha code.
const ISO_4217: Record<number, string> = {
  36: 'AUD', 124: 'CAD', 156: 'CNY', 208: 'DKK', 344: 'HKD', 356: 'INR', 392: 'JPY',
  554: 'NZD', 578: 'NOK', 702: 'SGD', 710: 'ZAR', 752: 'SEK', 756: 'CHF', 826: 'GBP',
  840: 'USD', 978: 'EUR', 985: 'PLN',
}

// Format an amount in major currency units, e.g. £1.23. Unknown or missing
// currencies fall back to a plain number.
export function fmtMoney(amount: number, currency?: number, fractionDigits = 2): string {
  const code = currency !== undefined ? ISO_4217[currency] : undefined
  if (code) {
    try {
      return new Intl.NumberFormat(undefined, {
        style: 'currency', currency: code,
        minimumFractionDigits: fractionDigits, maximumFractionDigits: fractionDigits,
      }).format(amount)
    } catch { /* fall through */ }
  }
  return amount.toFixed(fractionDigits)
}

// Price per kWh for a slot in major currency units, or null when unpriced.
export function slotPrice(day: TariffDay, slot: number): number | null {
  const v = day.slots[slot]
  if (v === null || v === undefined) return null
  return v / Math.pow(10, day.decimals ?? 0)
}

// Enough precision to show pence/cents on per-kWh prices (e.g. £0.245).
export function fmtUnitPrice(price: number, currency?: number): string {
  return `${fmtMoney(price, currency, Math.abs(price) < 1 ? 3 : 2)}/kWh`
}

export function currentSlot(now = new Date()): number {
  return Math.floor((now.getHours() * 60 + now.getMinutes()) / SLOT_MINUTES)
}

export function localDateString(d: Date): string {
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`
}
