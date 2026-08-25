// 1. Navigation Tab Switching with robust event delegation
document.addEventListener('click', (e) => {
  let target = e.target;
  let item = null;
  while (target && target !== document.body) {
    if (target.classList && target.classList.contains('nav-item')) {
      item = target;
      break;
    }
    target = target.parentNode;
  }
  if (!item) return;
  const tabName = item.getAttribute('data-tab');
  if (!tabName) return;

  const navItems = document.querySelectorAll('.nav-item');
  const tabPanels = document.querySelectorAll('.tab-panel');

  navItems.forEach(n => {
    n.classList.remove('active');
    n.style.backgroundColor = 'transparent';
    n.style.color = 'var(--text-secondary)';
  });
  tabPanels.forEach(p => {
    p.classList.remove('active');
    p.style.display = 'none';
  });

  item.classList.add('active');
  item.style.backgroundColor = 'var(--accent-color)';
  item.style.color = '#ffffff';

  const panel = document.getElementById('panel-' + tabName);
  if (panel) {
    panel.classList.add('active');
    panel.style.display = 'block';
  }
});

// 2. Dynamic Title Modification
const btnApplyTitle = document.getElementById('btn-apply-title');
const inputAppTitle = document.getElementById('input-app-title');
const displayTitle = document.getElementById('display-title');

if (btnApplyTitle) {
  btnApplyTitle.addEventListener('click', () => {
    const val = inputAppTitle.value.trim();
    if (val) {
      document.title = val;
      displayTitle.textContent = val;
    }
  });
}

// 3. Dynamic Theme Switcher
const btnDark = document.getElementById('btn-theme-dark');
const btnPurple = document.getElementById('btn-theme-purple');
const btnSunset = document.getElementById('btn-theme-sunset');

if (btnDark) {
  btnDark.addEventListener('click', () => {
    document.documentElement.style.setProperty('--bg-primary', '#121418');
    document.documentElement.style.setProperty('--bg-secondary', '#1a1d24');
    document.documentElement.style.setProperty('--bg-card', '#222630');
    document.documentElement.style.setProperty('--accent-color', '#3b82f6');
  });
}
if (btnPurple) {
  btnPurple.addEventListener('click', () => {
    document.documentElement.style.setProperty('--bg-primary', '#180b29');
    document.documentElement.style.setProperty('--bg-secondary', '#24123d');
    document.documentElement.style.setProperty('--bg-card', '#301852');
    document.documentElement.style.setProperty('--accent-color', '#a855f7');
  });
}
if (btnSunset) {
  btnSunset.addEventListener('click', () => {
    document.documentElement.style.setProperty('--bg-primary', '#1f130f');
    document.documentElement.style.setProperty('--bg-secondary', '#2b1b15');
    document.documentElement.style.setProperty('--bg-card', '#3d261e');
    document.documentElement.style.setProperty('--accent-color', '#f97316');
  });
}

// 4. Interactive Canvas Particle Animation
const canvas = document.getElementById('interactive-canvas');
let ctx = canvas ? canvas.getContext('2d') : null;
let particles = [];

function spawnParticles(count = 40) {
  if (!canvas) return;
  for (let i = 0; i < count; i++) {
    particles.push({
      x: canvas.width / 2,
      y: canvas.height / 2,
      vx: (Math.random() - 0.5) * 6,
      vy: (Math.random() - 0.5) * 6,
      radius: Math.random() * 4 + 2,
      color: `hsl(${Math.random() * 360}, 80%, 60%)`,
      life: 1.0
    });
  }
}

function updateParticles() {
  if (!ctx || !canvas) return;
  ctx.fillStyle = 'rgba(15, 17, 21, 0.2)';
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  for (let i = particles.length - 1; i >= 0; i--) {
    let p = particles[i];
    p.x += p.vx;
    p.y += p.vy;
    p.life -= 0.015;

    ctx.beginPath();
    ctx.arc(p.x, p.y, p.radius, 0, Math.PI * 2);
    ctx.fillStyle = p.color;
    ctx.globalAlpha = Math.max(0, p.life);
    ctx.fill();
    ctx.globalAlpha = 1.0;

    if (p.life <= 0 || p.x < 0 || p.x > canvas.width || p.y < 0 || p.y > canvas.height) {
      particles.splice(i, 1);
    }
  }

  requestAnimationFrame(updateParticles);
}

if (canvas && ctx) {
  spawnParticles(20);
  updateParticles();
  document.getElementById('btn-spawn-particles').addEventListener('click', () => spawnParticles(50));
  document.getElementById('btn-clear-canvas').addEventListener('click', () => {
    particles = [];
    ctx.fillStyle = '#0f1115';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  });
}

// 5. Native Web Audio Engine
let audioCtx = null;
function getAudioContext() {
  if (!audioCtx) {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  }
  if (audioCtx.state === 'suspended') audioCtx.resume();
  return audioCtx;
}

const btnSine = document.getElementById('btn-sound-sine');
const btnSynth = document.getElementById('btn-sound-synth');
const btnNoise = document.getElementById('btn-sound-noise');

if (btnSine) {
  btnSine.addEventListener('click', () => {
    const actx = getAudioContext();
    const osc = actx.createOscillator();
    const gain = actx.createGain();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(440, actx.currentTime);
    gain.gain.setValueAtTime(0.3, actx.currentTime);
    gain.gain.linearRampToValueAtTime(0.01, actx.currentTime + 0.5);
    osc.connect(gain).connect(actx.destination);
    osc.start();
    osc.stop(actx.currentTime + 0.5);
  });
}

if (btnSynth) {
  btnSynth.addEventListener('click', () => {
    const actx = getAudioContext();
    [523.25, 659.25, 783.99].forEach((freq, idx) => {
      const osc = actx.createOscillator();
      const gain = actx.createGain();
      osc.type = 'triangle';
      osc.frequency.setValueAtTime(freq, actx.currentTime + idx * 0.08);
      gain.gain.setValueAtTime(0.2, actx.currentTime + idx * 0.08);
      gain.gain.linearRampToValueAtTime(0.01, actx.currentTime + 0.8);
      osc.connect(gain).connect(actx.destination);
      osc.start(actx.currentTime + idx * 0.08);
      osc.stop(actx.currentTime + 0.8);
    });
  });
}

if (btnNoise) {
  btnNoise.addEventListener('click', () => {
    const actx = getAudioContext();
    const bufSize = actx.sampleRate * 0.6;
    const buf = actx.createBuffer(1, bufSize, actx.sampleRate);
    const data = buf.getChannelData(0);
    for (let i = 0; i < bufSize; i++) data[i] = (Math.random() * 2 - 1) * 0.2;
    const src = actx.createBufferSource();
    src.buffer = buf;
    const filter = actx.createBiquadFilter();
    filter.type = 'lowpass';
    filter.frequency.setValueAtTime(800, actx.currentTime);
    src.connect(filter).connect(actx.destination);
    src.start();
  });
}
