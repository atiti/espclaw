const REPO = "atiti/espclaw";
const RELEASE_API = `https://api.github.com/repos/${REPO}/releases/latest`;

const TARGETS = [
  {
    id: "esp32",
    title: "ESP32 / ESP32-CAM",
    subtitle: "Camera-first compatibility profile",
    chip: "ESP32",
    notes: [
      "Best fit for AI Thinker ESP32-CAM and similar PSRAM-equipped ESP32 boards.",
      "Includes OTA-ready partition layout and browser-flash manifest.",
    ],
  },
  {
    id: "esp32s3",
    title: "ESP32-S3",
    subtitle: "Primary full-agent profile",
    chip: "ESP32-S3",
    notes: [
      "Recommended for the fullest ESPClaw experience and headroom.",
      "Best option if you want the broadest feature margin for apps, tools, and chat.",
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

function renderBoardCard(target, assets, release) {
  const manifest = assets.get(`esp-web-tools-manifest-${target.id}.json`);
  const bundle = [...assets.values()].find(
    (asset) => asset.name.startsWith(`espclaw-${target.id}-${release.tag_name}`) && asset.name.endsWith(".zip"),
  );

  const article = document.createElement("article");
  article.className = "board-card";

  const install = manifest
    ? `<esp-web-install-button manifest="${manifest.browser_download_url}"></esp-web-install-button>`
    : `<p class="error">No install manifest published for this target in the latest release.</p>`;

  const download = bundle
    ? `<a class="button tertiary" href="${bundle.browser_download_url}">Download bundle</a>`
    : `<span class="muted">Bundle missing</span>`;

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
      ${install}
      ${download}
    </div>
    <ul class="board-notes">
      ${target.notes.map((note) => `<li>${note}</li>`).join("")}
    </ul>
  `;

  return article;
}

async function loadRelease() {
  const panelTitle = document.getElementById("release-title");
  const panelSummary = document.getElementById("release-summary");
  const panelTag = document.getElementById("release-tag");
  const panelDate = document.getElementById("release-date");
  const boardGrid = document.getElementById("board-grid");

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

    panelTitle.textContent = release.name || `Latest release ${release.tag_name}`;
    panelSummary.textContent = release.body
      ? release.body.split("\n").find((line) => line.trim()) || "Fresh firmware artifacts are available."
      : "Fresh firmware artifacts are available.";
    panelTag.textContent = release.tag_name;
    panelDate.textContent = formatDate(release.published_at);

    boardGrid.innerHTML = "";
    for (const target of TARGETS) {
      boardGrid.appendChild(renderBoardCard(target, assets, release));
    }
  } catch (error) {
    panelTitle.textContent = "Release lookup failed";
    panelSummary.textContent = "The flasher page could not read the latest GitHub release metadata.";
    panelTag.textContent = "error";
    panelDate.textContent = "—";
    boardGrid.innerHTML = `
      <article class="board-card board-card--error">
        <p class="eyebrow">RELEASE API</p>
        <h3>Could not load the latest release.</h3>
        <p>${error.message}</p>
        <a class="button tertiary" href="https://github.com/${REPO}/releases/latest">Open GitHub Releases</a>
      </article>
    `;
  }
}

loadRelease();
