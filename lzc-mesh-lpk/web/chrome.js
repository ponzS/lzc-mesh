"use strict"
// LightOS uses declarative host chrome, not forced fullscreen. Host-provided
// safe areas (including explicit 0px) take precedence over browser env() values.
{
  const dark = matchMedia("(prefers-color-scheme: dark)")
  const update = () => document.querySelector('meta[name="theme-color"]')?.setAttribute("content", dark.matches ? "#111111" : "#f5f5f5")
  dark.addEventListener("change", update)
  update()
  // This identifies a host bridge contract, not the device's input capability.
  if (navigator.userAgent.includes("Lazycat_103")) document.documentElement.classList.add("lzc-ios-host")
}
