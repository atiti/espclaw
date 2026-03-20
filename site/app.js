import { BrowserLab } from "./simulator.js";

const REPO = "atiti/espclaw";
const RELEASE_API = `https://api.github.com/repos/${REPO}/releases/latest`;

const TARGETS = [
  {
    id: "esp32",
    title: "ESP32 / ESP32-CAM",
    subtitle: "Camera-first profile for compact boards and low-cost experimentation.",
    chip: "ESP32",
    notes: [
      "Best fit for AI Thinker ESP32-CAM and similar PSRAM-equipped ESP32 boards.",
      "Ships with OTA-ready partitions and a browser manifest for first flash.",
      "Good low-cost starting point for camera, GPIO, and Telegram-driven flows.",
    ],
  },
  {
    id: "esp32s3",
    title: "ESP32-S3",
    subtitle: "Higher-headroom profile for broader experimentation and larger apps.",
    chip: "ESP32-S3",
    notes: [
      "Recommended if you want the fullest ESPClaw feature margin and memory headroom.",
      "Best option for heavier local tooling, more ambitious apps, and broader debugging.",
      "Uses the same release and browser-flash path as the ESP32 target.",
    ],
  },
];

function assetMap(assets) {
  return new Map(assets.map((asset) => [asset.name, asset]));
}

function formatDate(value) {
  return new Intl.DateTimeFormat("en", {
    year: "numeric",
    month: "short",
    day: "numeric",
  }).format(new Date(value));
}

function firstReleaseLine(body) {
  if (!body) {
    return "Fresh firmware artifacts are available from GitHub Releases.";
  }
  return (
    body
      .split("\n")
      .map((line) => line.trim())
      .find((line) => line && !line.startsWith("#")) || "Fresh firmware artifacts are available from GitHub Releases."
  );
}

function renderBoardCard(target, assets, release) {
  const manifest = assets.get(`esp-web-tools-manifest-${target.id}.json`);
  const bundle = [...assets.values()].find(
    (asset) => asset.name.startsWith(`espclaw-${target.id}-${release.tag_name}`) && asset.name.endsWith(".zip"),
  );

  const article = document.createElement("article");
  article.className = "board-card";
  article.innerHTML = `
    <div class="board-head">
      <div>
        <p class="eyebrow">${target.chip}</p>
        <h3>${target.title}</h3>
        <p class="subtitle">${target.subtitle}</p>
      </div>
      <span class="pill">${target.id}</span>
    </div>
    <div class="board-actions">
      ${
        manifest
          ? `<esp-web-install-button manifest="${manifest.browser_download_url}"></esp-web-install-button>`
          : `<span class="error">Install manifest missing</span>`
      }
      ${
        bundle
          ? `<a class="button button-ghost" href="${bundle.browser_download_url}">Download bundle</a>`
          : `<span class="error">Bundle missing</span>`
      }
    </div>
    <ul>
      ${target.notes.map((note) => `<li>${note}</li>`).join("")}
    </ul>
  `;
  return article;
}

function markActiveNav() {
  const page = document.body.dataset.page;
  document.querySelectorAll("[data-nav]").forEach((link) => {
    link.classList.toggle("is-active", link.dataset.nav === page);
  });
}

async function loadRelease() {
  const boardGrid = document.getElementById("board-grid");
  const releaseSummary = document.getElementById("release-summary");
  const releaseTag = document.getElementById("release-tag");
  const releaseDate = document.getElementById("release-date");

  if (!boardGrid) {
    return;
  }

  try {
    const response = await fetch(RELEASE_API, {
      headers: {
        Accept: "application/vnd.github+json",
      },
    });
    if (!response.ok) {
      throw new Error(`GitHub API returned ${response.status}`);
    }
    const release = await response.json();
    const assets = assetMap(release.assets || []);

    if (releaseTag) {
      releaseTag.textContent = release.tag_name;
    }
    if (releaseDate) {
      releaseDate.textContent = formatDate(release.published_at);
    }
    if (releaseSummary) {
      releaseSummary.textContent = firstReleaseLine(release.body);
    }

    boardGrid.innerHTML = "";
    for (const target of TARGETS) {
      boardGrid.appendChild(renderBoardCard(target, assets, release));
    }
  } catch (error) {
    if (releaseTag) {
      releaseTag.textContent = "error";
    }
    if (releaseDate) {
      releaseDate.textContent = "—";
    }
    if (releaseSummary) {
      releaseSummary.textContent = "The page could not read the latest GitHub release metadata.";
    }
    boardGrid.innerHTML = `
      <article class="board-card board-card--error">
        <p class="eyebrow">RELEASE API</p>
        <h3>Could not load the latest release.</h3>
        <p>${error.message}</p>
        <a class="button button-ghost" href="https://github.com/${REPO}/releases/latest">Open GitHub Releases</a>
      </article>
    `;
  }
}

function bootLab() {
  if (document.body.dataset.page !== "lab") {
    return;
  }
  new BrowserLab(document);
}

function boot() {
  markActiveNav();
  bootLab();
  void loadRelease();
}

boot();
