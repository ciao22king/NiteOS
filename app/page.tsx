"use client"

import { useState } from "react"
import { ArrowUpRight, Check, ChevronRight, Cpu, Github, Menu, Network, Play, Shield, Terminal, X } from "lucide-react"

const features = [
  { icon: Cpu, number: "01", title: "Bare metal, no shortcuts", copy: "An x86-64 kernel that boots through Multiboot2, owns its memory, and speaks directly to the hardware." },
  { icon: Network, number: "02", title: "A network-native core", copy: "DHCP, DNS, TCP, HTTP and an e1000 driver — enough of the internet to make the machine feel alive." },
  { icon: Shield, number: "03", title: "A filesystem you can trust", copy: "NEOFS mounts, reads, writes and deletes on disk, with first-boot driver acquisition and SHA-256 verification." },
]

const commands = ["report", "cpu", "mem", "pci", "gfx", "net", "fs", "run hello_elf"]

export default function Home() {
  const [menuOpen, setMenuOpen] = useState(false)
  const [activeCommand, setActiveCommand] = useState("report")

  return (
    <main>
      <nav className="nav shell">
        <a className="brand" href="#top" aria-label="NiteOS home"><span className="brand-mark">N</span><span>Nite<span>OS</span></span></a>
        <div className={`nav-links ${menuOpen ? "is-open" : ""}`}>
          <a href="#inside" onClick={() => setMenuOpen(false)}>Inside NiteOS</a>
          <a href="#stack" onClick={() => setMenuOpen(false)}>The stack</a>
          <a href="https://github.com/ciao22king/NiteOS" target="_blank" rel="noreferrer" onClick={() => setMenuOpen(false)}>GitHub <ArrowUpRight size={14} /></a>
        </div>
        <a className="nav-cta" href="#quickstart">Explore the kernel <ChevronRight size={15} /></a>
        <button className="menu-button" aria-label={menuOpen ? "Close menu" : "Open menu"} onClick={() => setMenuOpen(!menuOpen)}>{menuOpen ? <X /> : <Menu />}</button>
      </nav>

      <section className="hero shell" id="top">
        <div className="hero-copy">
          <p className="eyebrow"><span className="pulse" /> x86-64 / open source / v0.1</p>
          <h1>The machine is<br /><em>yours</em> again.</h1>
          <p className="hero-lede">NiteOS is a from-scratch operating system for people who want to understand the whole stack — from the first instruction to the first HTTP request.</p>
          <div className="hero-actions">
            <a className="button primary" href="#quickstart"><Play size={16} fill="currentColor" /> See it boot</a>
            <a className="text-link" href="https://github.com/ciao22king/NiteOS" target="_blank" rel="noreferrer">Read the source <ArrowUpRight size={15} /></a>
          </div>
          <div className="hero-proof"><div className="avatars"><span>G</span><span>Q</span><span>H</span></div><span>Built in public, one subsystem at a time.</span></div>
        </div>
        <div className="hero-terminal" aria-label="NiteOS terminal preview">
          <div className="terminal-top"><span className="window-dots"><i /><i /><i /></span><span>nite@qemu:~</span><span className="terminal-status">● online</span></div>
          <div className="terminal-body"><div className="terminal-line muted">NiteKernel / multiboot2 / 64-bit</div><div className="terminal-line">&gt; <strong>boot nitekernel.elf</strong></div><div className="terminal-line success">[ ok ] memory manager online</div><div className="terminal-line success">[ ok ] e1000 network device found</div><div className="terminal-line success">[ ok ] neofs volume mounted</div><div className="terminal-line blank" /><div className="terminal-line cyan">nite:10.0.2.15&gt; <span className="typing">report</span></div><div className="terminal-grid"><span>cpu</span><b>x86_64</b><span>memory</span><b>512 MB</b><span>devices</span><b>04 found</b><span>uptime</span><b>00:00:42</b></div><div className="terminal-line cyan">nite:10.0.2.15&gt; <span className="cursor" /></div></div>
          <div className="terminal-glow" />
        </div>
      </section>

      <section className="marquee"><div className="marquee-inner"><span>MEMORY MANAGEMENT</span><i>✳</i><span>NETWORK STACK</span><i>✳</i><span>NEOFS</span><i>✳</i><span>RING 3 PROCESSES</span><i>✳</i><span>MEMORY MANAGEMENT</span></div></section>

      <section className="section shell" id="inside"><div className="section-heading"><p className="eyebrow">01 / What&apos;s inside</p><h2>Small enough to<br /><em>understand.</em></h2><p className="section-intro">No layers of abstraction between you and the interesting parts. NiteOS makes every subsystem visible, inspectable and yours to change.</p></div><div className="feature-grid">{features.map(({ icon: Icon, number, title, copy }) => <article className="feature" key={number}><div className="feature-top"><span className="feature-number">{number}</span><Icon size={22} strokeWidth={1.5} /></div><h3>{title}</h3><p>{copy}</p><a href="#stack">Explore subsystem <ChevronRight size={15} /></a></article>)}</div></section>

      <section className="command-section" id="stack"><div className="shell command-layout"><div><p className="eyebrow">02 / The shell</p><h2>Your computer,<br /><em>talking back.</em></h2><p className="section-intro">A real interactive shell with pipes, redirection, scripts and enough introspection to tell you exactly what the machine is doing.</p><div className="command-list">{commands.map((command) => <button key={command} className={activeCommand === command ? "active" : ""} onClick={() => setActiveCommand(command)}><Terminal size={15} />{command}<ChevronRight size={14} /></button>)}</div></div><div className="command-output"><div className="output-head"><span>command output</span><span>serial / stdio</span></div><div className="output-body"><p className="cyan">nite:10.0.2.15&gt; {activeCommand}</p><p className="output-title">{activeCommand === "report" ? "NITEKERNEL SYSTEM REPORT" : activeCommand.toUpperCase()}</p><p className="muted">──────────────────────────────</p><p>architecture <b>x86-64</b></p><p>kernel      <b>nitekernel</b></p><p>interrupts  <b>gdt / idt / pic</b></p><p>filesystem   <b>NEOFS mounted</b></p><p>network      <b>e1000 / dhcp</b></p><p className="success">status       all systems nominal</p><span className="output-cursor">▋</span></div></div></div></section>

      <section className="quickstart shell" id="quickstart"><div><p className="eyebrow">03 / Start here</p><h2>Boot something<br /><em>real.</em></h2></div><div className="quickstart-card"><div className="quickstart-copy"><span className="step">01</span><div><h3>Build the image</h3><p>Everything you need is in the repository. Bring a normal host toolchain and QEMU.</p></div></div><code><span>$</span> make<br /><span className="comment"># build/nitekernel.iso</span></code><div className="quickstart-copy"><span className="step">02</span><div><h3>Run it</h3><p>Drop into the serial console and start exploring.</p></div></div><code><span>$</span> make run</code><a className="button dark" href="https://github.com/ciao22king/NiteOS#building-and-running" target="_blank" rel="noreferrer">Full build guide <ArrowUpRight size={15} /></a></div></section>

      <footer className="footer shell"><a className="brand" href="#top"><span className="brand-mark">N</span><span>Nite<span>OS</span></span></a><p>Built from first principles.</p><a href="https://github.com/ciao22king/NiteOS" target="_blank" rel="noreferrer"><Github size={17} /> GitHub</a></footer>
    </main>
  )
}
