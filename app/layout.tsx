import type { Metadata } from "next"
import "./globals.css"

export const metadata: Metadata = {
  title: "NiteOS — The small kernel with big ambitions",
  description: "A from-scratch x86-64 operating system built for curiosity, control, and the open machine.",
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="en"><body>{children}</body></html>
}
