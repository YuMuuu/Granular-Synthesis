import path from 'node:path'
import { mkdir, writeFile } from 'node:fs/promises'
import { chromium } from 'playwright'

const storybookUrl = process.env.STORYBOOK_URL ?? 'http://127.0.0.1:6006'
const outputDir = process.env.STORYBOOK_SCREENSHOT_DIR ?? 'screenshots/storybook'

const pluginViewports = [
  { name: 'min', width: 492, height: 238 },
  { name: 'standard', width: 800, height: 400 },
  { name: 'wide', width: 1200, height: 700 },
]

const knobViewports = [
  { name: 'compact', width: 240, height: 240 },
  { name: 'standard', width: 400, height: 320 },
]

const targets = [
  { id: 'plugin-interface--default', name: 'interface-default', viewports: pluginViewports },
  { id: 'plugin-interface--dry-small-room', name: 'interface-dry-small-room', viewports: pluginViewports },
  { id: 'plugin-interface--wide-lush', name: 'interface-wide-lush', viewports: pluginViewports },
  { id: 'plugin-interface--with-error', name: 'interface-with-error', viewports: pluginViewports },
  { id: 'controls-knob--default', name: 'knob-default', viewports: knobViewports },
  { id: 'controls-knob--minimum', name: 'knob-minimum', viewports: knobViewports },
  { id: 'controls-knob--maximum', name: 'knob-maximum', viewports: knobViewports },
]

function storyUrl(storyId) {
  const url = new URL('/iframe.html', storybookUrl)
  url.searchParams.set('id', storyId)
  url.searchParams.set('viewMode', 'story')
  return url.toString()
}

async function collectLayoutInfo(page) {
  return page.evaluate(() => {
    const rectToObject = (rect) => ({
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height,
      top: rect.top,
      right: rect.right,
      bottom: rect.bottom,
      left: rect.left,
    })

    const storyRoot = document.querySelector('#storybook-root')
    const canvases = Array.from(document.querySelectorAll('canvas')).map((canvas) => {
      const ctx = canvas.getContext('2d')
      const imageData = ctx && canvas.width > 0 && canvas.height > 0
        ? ctx.getImageData(0, 0, canvas.width, canvas.height).data
        : null
      let nonTransparentPixels = 0

      if (imageData) {
        for (let i = 3; i < imageData.length; i += 4) {
          if (imageData[i] > 0) {
            nonTransparentPixels += 1
          }
        }
      }

      return {
        box: rectToObject(canvas.getBoundingClientRect()),
        width: canvas.width,
        height: canvas.height,
        nonTransparentPixels,
      }
    })

    return {
      viewport: {
        width: document.documentElement.clientWidth,
        height: document.documentElement.clientHeight,
      },
      body: {
        scrollWidth: document.body.scrollWidth,
        scrollHeight: document.body.scrollHeight,
      },
      storyRoot: storyRoot ? rectToObject(storyRoot.getBoundingClientRect()) : null,
      canvases,
    }
  })
}

async function main() {
  await mkdir(outputDir, { recursive: true })

  const browser = await chromium.launch()
  const manifest = []

  try {
    for (const target of targets) {
      for (const viewport of target.viewports) {
        const page = await browser.newPage({
          viewport: {
            width: viewport.width,
            height: viewport.height,
          },
          deviceScaleFactor: 1,
        })

        const url = storyUrl(target.id)
        await page.goto(url, { waitUntil: 'networkidle' })
        await page.waitForSelector('#storybook-root')
        await page.waitForTimeout(250)

        const filename = `${target.name}-${viewport.name}-${viewport.width}x${viewport.height}.png`
        const screenshotPath = path.join(outputDir, filename)
        await page.screenshot({ path: screenshotPath })

        manifest.push({
          storyId: target.id,
          storyName: target.name,
          viewport,
          url,
          screenshotPath,
          layout: await collectLayoutInfo(page),
        })

        await page.close()
      }
    }
  } finally {
    await browser.close()
  }

  const manifestPath = path.join(outputDir, 'manifest.json')
  await writeFile(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`)

  console.log(`Saved ${manifest.length} screenshots to ${outputDir}`)
  console.log(`Wrote ${manifestPath}`)
}

main().catch((error) => {
  console.error(error)
  process.exitCode = 1
})
