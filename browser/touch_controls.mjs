// Mobile Touch Controls for Star Wars Battlefront II (Android & iOS).
//
// Context-aware dual-mode multi-touch controller:
//   - Menu / Spawn mode (!pointerLockWanted): unobstructed direct tap-to-click on the canvas
//     (letterbox-corrected) plus compact Enter / Esc helper buttons.
//   - Live 3D Combat mode (pointerLockWanted):
//       * Left half: dynamic floating virtual thumbstick (8-way WASD + auto-sprint at >85% forward)
//       * Right half: full-area smooth touch-look camera aim (no PointerLock needed — works on iOS!)
//       * Ergonomic right-thumb action arc: FIRE (with drag-to-aim!), ALT/GRENADE (RMB), JUMP,
//         CROUCH/ROLL, RELOAD, USE/VEHICLE, ZOOM, SWAP PRIMARY/SECONDARY
//       * Top utility buttons: PAUSE (Esc), SCOREBOARD (Tab)

export function detectTouchDevice() {
  if (typeof window === 'undefined') return false;
  const coarse = typeof window.matchMedia === 'function' && window.matchMedia('(pointer: coarse)').matches;
  const touchPts = (navigator.maxTouchPoints || 0) > 0 || ('ontouchstart' in window);
  const mobileUa = /Android|iPhone|iPad|iPod/i.test(navigator.userAgent || '');
  return !!(mobileUa || (coarse && touchPts));
}

function haptic(ms = 10) {
  try {
    if (navigator.vibrate) navigator.vibrate(ms);
  } catch {}
}

export class TouchControls {
  constructor({
    rootEl,
    canvas,
    getInputWriter,
    mode = 'auto', // 'auto' | 'on' | 'off'
    sensitivity = 1.2,
    onResumeAudio = null,
  } = {}) {
    this.root = rootEl;
    this.canvas = canvas;
    this.getInputWriter = getInputWriter;
    this.mode = mode;
    this.sensitivity = Number.isFinite(sensitivity) ? sensitivity : 1.2;
    this.onResumeAudio = onResumeAudio;

    this.isTouchDevice = detectTouchDevice();
    this.booted = false;
    this.combatMode = false;
    this.overlayBlocked = false;
    this.portraitDismissed = false;

    // Multi-touch pointer tracking
    this.stickPointerId = null;
    this.stickOriginX = 0;
    this.stickOriginY = 0;
    this.stickMaxRadius = 56;
    this.stickKeys = { KeyW: false, KeyA: false, KeyS: false, KeyD: false, ShiftLeft: false };

    this.lookPointerId = null;
    this.lookLastX = 0;
    this.lookLastY = 0;

    this.firePointerId = null;
    this.fireLastX = 0;
    this.fireLastY = 0;

    this.menuPointerId = null;
    this.activeBtnPointers = new Map(); // pointerId -> { releaseFn, el }

    this.elCombat = rootEl?.querySelector('#tcCombat') || null;
    this.elMenu = rootEl?.querySelector('#tcMenu') || null;
    this.elStickZone = rootEl?.querySelector('#tcStickZone') || null;
    this.elStickBase = rootEl?.querySelector('#tcStickBase') || null;
    this.elStickKnob = rootEl?.querySelector('#tcStickKnob') || null;
    this.elSprintTag = rootEl?.querySelector('#tcSprintTag') || null;
    this.elLookZone = rootEl?.querySelector('#tcLookZone') || null;
    this.elFireBtn = rootEl?.querySelector('#tcBtnFire') || null;
    this.elRotateHint = rootEl?.querySelector('#tcRotateHint') || null;

    this._onResize = this._onResize.bind(this);
    this._onCanvasPointerDown = this._onCanvasPointerDown.bind(this);
    this._onCanvasPointerMove = this._onCanvasPointerMove.bind(this);
    this._onCanvasPointerUp = this._onCanvasPointerUp.bind(this);
    this._onWindowPointerCancel = this._onWindowPointerCancel.bind(this);

    this._bindEvents();
    this._updateVisibility();
  }

  isEnabled() {
    if (this.mode === 'off') return false;
    if (this.mode === 'on') return true;
    // 'auto': enable on touch devices unless a physical Bluetooth/USB gamepad is actively connected.
    const writer = this.getInputWriter?.();
    if (writer && writer.hasPhysicalGamepad()) return false;
    return this.isTouchDevice;
  }

  setMode(mode) {
    this.mode = mode === 'on' || mode === 'off' ? mode : 'auto';
    if (!this.isEnabled()) this.releaseAll();
    this._updateVisibility();
  }

  setSensitivity(val) {
    const n = Number(val);
    if (Number.isFinite(n) && n >= 0.3 && n <= 3.0) {
      this.sensitivity = n;
    }
  }

  setState({ booted, combatMode, overlayBlocked } = {}) {
    let changed = false;
    if (booted !== undefined && booted !== this.booted) {
      this.booted = !!booted;
      changed = true;
    }
    if (combatMode !== undefined && combatMode !== this.combatMode) {
      this.combatMode = !!combatMode;
      changed = true;
      // Release any held combat keys/buttons when transitioning to/from menus or spawn screen.
      this.releaseAll();
    }
    if (overlayBlocked !== undefined && overlayBlocked !== this.overlayBlocked) {
      this.overlayBlocked = !!overlayBlocked;
      changed = true;
      if (this.overlayBlocked) this.releaseAll();
    }
    if (changed) this._updateVisibility();
  }

  _updateVisibility() {
    if (!this.root) return;
    const active = this.booted && this.isEnabled() && !this.overlayBlocked;
    this.root.classList.toggle('active', active);
    if (this.elCombat) this.elCombat.style.display = (active && this.combatMode) ? 'block' : 'none';
    if (this.elMenu) this.elMenu.style.display = (active && !this.combatMode) ? 'flex' : 'none';

    if (this.elRotateHint) {
      const isPortrait = window.innerHeight > window.innerWidth * 1.05;
      const smallPhone = Math.min(window.innerWidth, window.innerHeight) <= 560;
      const showRotate = active && this.isTouchDevice && isPortrait && smallPhone && !this.portraitDismissed;
      this.elRotateHint.classList.toggle('open', showRotate);
    }
  }

  _onResize() {
    this._updateVisibility();
  }

  releaseAll() {
    const writer = this.getInputWriter?.();
    this._resetStick();
    if (this.firePointerId !== null) {
      this.firePointerId = null;
      this.elFireBtn?.classList.remove('pressed');
      writer?.setMouseButton(1, false);
    }
    this.lookPointerId = null;
    if (this.menuPointerId !== null) {
      this.menuPointerId = null;
      writer?.setMouseButton(1, false);
    }
    for (const [, entry] of this.activeBtnPointers) {
      try { entry.releaseFn(); } catch {}
      entry.el?.classList.remove('pressed');
    }
    this.activeBtnPointers.clear();
  }

  _resetStick() {
    const writer = this.getInputWriter?.();
    this.stickPointerId = null;
    for (const k of Object.keys(this.stickKeys)) {
      if (this.stickKeys[k]) {
        this.stickKeys[k] = false;
        writer?.setKey(k, false);
      }
    }
    if (this.elStickBase) {
      this.elStickBase.classList.remove('active');
      this.elStickBase.style.left = '';
      this.elStickBase.style.top = '';
    }
    if (this.elStickKnob) {
      this.elStickKnob.style.transform = 'translate(-50%, -50%)';
    }
    if (this.elSprintTag) {
      this.elSprintTag.classList.remove('on');
    }
  }

  _updateStick(clientX, clientY) {
    const writer = this.getInputWriter?.();
    if (!writer) return;
    const dx = clientX - this.stickOriginX;
    const dy = clientY - this.stickOriginY;
    const dist = Math.hypot(dx, dy);
    const maxR = this.stickMaxRadius;
    const clamped = Math.min(dist, maxR);
    const ang = Math.atan2(dy, dx);
    const kx = Math.cos(ang) * clamped;
    const ky = Math.sin(ang) * clamped;

    if (this.elStickKnob) {
      this.elStickKnob.style.transform = `translate(calc(-50% + ${kx.toFixed(1)}px), calc(-50% + ${ky.toFixed(1)}px))`;
    }

    const nx = dx / maxR;
    const ny = dy / maxR;
    const dead = 0.24;
    const next = {
      KeyW: ny < -dead,
      KeyS: ny > dead,
      KeyA: nx < -dead,
      KeyD: nx > dead,
      ShiftLeft: ny < -0.86 && Math.abs(nx) < 0.65,
    };
    for (const [k, v] of Object.entries(next)) {
      if (this.stickKeys[k] !== v) {
        this.stickKeys[k] = v;
        writer.setKey(k, v);
      }
    }
    if (this.elSprintTag) {
      this.elSprintTag.classList.toggle('on', next.ShiftLeft);
    }
  }

  _bindEvents() {
    if (!this.root) return;

    window.addEventListener('resize', this._onResize);
    window.addEventListener('orientationchange', this._onResize);
    window.addEventListener('blur', this._onWindowPointerCancel);

    // Dismiss portrait rotation hint
    this.root.querySelector('#tcRotateDismiss')?.addEventListener('click', () => {
      this.portraitDismissed = true;
      this._updateVisibility();
    });

    // ---- 1. Direct Tap-to-Click on Canvas for Menus / Spawn Screen ----
    if (this.canvas) {
      this.canvas.addEventListener('pointerdown', this._onCanvasPointerDown, { passive: false });
      this.canvas.addEventListener('pointermove', this._onCanvasPointerMove, { passive: false });
      this.canvas.addEventListener('pointerup', this._onCanvasPointerUp, { passive: false });
      this.canvas.addEventListener('pointercancel', this._onCanvasPointerUp, { passive: false });
    }

    // ---- 2. Left Virtual Thumbstick (Floating + Auto-Sprint) ----
    if (this.elStickZone) {
      this.elStickZone.addEventListener('pointerdown', (ev) => {
        if (!this.combatMode || !this.isEnabled()) return;
        if (this.stickPointerId !== null) return;
        ev.preventDefault();
        this.onResumeAudio?.();
        this.stickPointerId = ev.pointerId;
        try { this.elStickZone.setPointerCapture(ev.pointerId); } catch {}
        const zoneRect = this.elStickZone.getBoundingClientRect();
        this.stickOriginX = ev.clientX;
        this.stickOriginY = ev.clientY;
        if (this.elStickBase) {
          this.elStickBase.style.left = `${ev.clientX - zoneRect.left}px`;
          this.elStickBase.style.top = `${ev.clientY - zoneRect.top}px`;
          this.elStickBase.classList.add('active');
        }
        this._updateStick(ev.clientX, ev.clientY);
      }, { passive: false });

      this.elStickZone.addEventListener('pointermove', (ev) => {
        if (ev.pointerId !== this.stickPointerId) return;
        ev.preventDefault();
        this._updateStick(ev.clientX, ev.clientY);
      }, { passive: false });

      const endStick = (ev) => {
        if (ev.pointerId !== this.stickPointerId) return;
        ev.preventDefault();
        this._resetStick();
      };
      this.elStickZone.addEventListener('pointerup', endStick, { passive: false });
      this.elStickZone.addEventListener('pointercancel', endStick, { passive: false });
    }

    // ---- 3. Right Touch-Look Aim Surface ----
    if (this.elLookZone) {
      this.elLookZone.addEventListener('pointerdown', (ev) => {
        if (!this.combatMode || !this.isEnabled()) return;
        if (this.lookPointerId !== null) return;
        ev.preventDefault();
        this.onResumeAudio?.();
        this.lookPointerId = ev.pointerId;
        this.lookLastX = ev.clientX;
        this.lookLastY = ev.clientY;
        try { this.elLookZone.setPointerCapture(ev.pointerId); } catch {}
      }, { passive: false });

      this.elLookZone.addEventListener('pointermove', (ev) => {
        if (ev.pointerId !== this.lookPointerId) return;
        ev.preventDefault();
        const dx = (ev.clientX - this.lookLastX) * this.sensitivity * 1.45;
        const dy = (ev.clientY - this.lookLastY) * this.sensitivity * 1.45;
        this.lookLastX = ev.clientX;
        this.lookLastY = ev.clientY;
        this.getInputWriter?.()?.addMouseDelta(dx, dy);
      }, { passive: false });

      const endLook = (ev) => {
        if (ev.pointerId !== this.lookPointerId) return;
        ev.preventDefault();
        this.lookPointerId = null;
      };
      this.elLookZone.addEventListener('pointerup', endLook, { passive: false });
      this.elLookZone.addEventListener('pointercancel', endLook, { passive: false });
    }

    // ---- 4. Primary FIRE Button (supports Drag-to-Aim while firing!) ----
    if (this.elFireBtn) {
      this.elFireBtn.addEventListener('pointerdown', (ev) => {
        if (!this.combatMode || !this.isEnabled()) return;
        if (this.firePointerId !== null) return;
        ev.preventDefault();
        ev.stopPropagation();
        this.onResumeAudio?.();
        haptic(12);
        this.firePointerId = ev.pointerId;
        this.fireLastX = ev.clientX;
        this.fireLastY = ev.clientY;
        this.elFireBtn.classList.add('pressed');
        try { this.elFireBtn.setPointerCapture(ev.pointerId); } catch {}
        this.getInputWriter?.()?.setMouseButton(1, true);
      }, { passive: false });

      this.elFireBtn.addEventListener('pointermove', (ev) => {
        if (ev.pointerId !== this.firePointerId) return;
        ev.preventDefault();
        const dx = (ev.clientX - this.fireLastX) * this.sensitivity * 1.35;
        const dy = (ev.clientY - this.fireLastY) * this.sensitivity * 1.35;
        this.fireLastX = ev.clientX;
        this.fireLastY = ev.clientY;
        this.getInputWriter?.()?.addMouseDelta(dx, dy);
      }, { passive: false });

      const endFire = (ev) => {
        if (ev.pointerId !== this.firePointerId) return;
        ev.preventDefault();
        this.firePointerId = null;
        this.elFireBtn.classList.remove('pressed');
        this.getInputWriter?.()?.setMouseButton(1, false);
      };
      this.elFireBtn.addEventListener('pointerup', endFire, { passive: false });
      this.elFireBtn.addEventListener('pointercancel', endFire, { passive: false });
    }

    // ---- 5. Ergonomic Action Buttons (Hold & Tap bindings) ----
    const bindHoldButton = (selector, pressFn, releaseFn, vibrateMs = 8) => {
      const el = this.root.querySelector(selector);
      if (!el) return;
      el.addEventListener('pointerdown', (ev) => {
        if (!this.isEnabled()) return;
        ev.preventDefault();
        ev.stopPropagation();
        this.onResumeAudio?.();
        if (vibrateMs > 0) haptic(vibrateMs);
        el.classList.add('pressed');
        try { el.setPointerCapture(ev.pointerId); } catch {}
        pressFn();
        this.activeBtnPointers.set(ev.pointerId, { releaseFn, el });
      }, { passive: false });

      const end = (ev) => {
        const entry = this.activeBtnPointers.get(ev.pointerId);
        if (!entry) return;
        ev.preventDefault();
        this.activeBtnPointers.delete(ev.pointerId);
        entry.el?.classList.remove('pressed');
        entry.releaseFn();
      };
      el.addEventListener('pointerup', end, { passive: false });
      el.addEventListener('pointercancel', end, { passive: false });
    };

    const keyHold = (selector, code, vib = 8) => bindHoldButton(
      selector,
      () => this.getInputWriter?.()?.setKey(code, true),
      () => this.getInputWriter?.()?.setKey(code, false),
      vib,
    );

    // Secondary Fire / Grenade / Force Power / Lightsaber Block (RMB)
    bindHoldButton(
      '#tcBtnAlt',
      () => this.getInputWriter?.()?.setMouseButton(2, true),
      () => this.getInputWriter?.()?.setMouseButton(2, false),
      10,
    );

    keyHold('#tcBtnJump', 'Space', 8);
    keyHold('#tcBtnCrouch', 'KeyC', 6);
    keyHold('#tcBtnReload', 'KeyR', 6);
    keyHold('#tcBtnUse', 'KeyE', 8);
    keyHold('#tcBtnZoom', 'KeyZ', 6);
    keyHold('#tcBtnSwapPri', 'KeyF', 6);
    keyHold('#tcBtnSwapSec', 'KeyQ', 6);
    keyHold('#tcBtnScore', 'Tab', 0);
    keyHold('#tcBtnPause', 'Escape', 6);
    keyHold('#tcBtnMenuEnter', 'Enter', 6);
    keyHold('#tcBtnMenuEsc', 'Escape', 6);
  }

  _onCanvasPointerDown(ev) {
    // Only handle touch/pen taps on the canvas when in Menu / Spawn screen mode!
    // Desktop mouse clicks continue through InputBridgeWriter's standard mousedown handler.
    if (ev.pointerType !== 'touch' && ev.pointerType !== 'pen') return;
    if (!this.booted || !this.isEnabled() || this.overlayBlocked || this.combatMode) return;
    ev.preventDefault();
    this.onResumeAudio?.();
    this.menuPointerId = ev.pointerId;
    try { this.canvas.setPointerCapture(ev.pointerId); } catch {}
    const writer = this.getInputWriter?.();
    writer?.setCursorDown(ev.clientX, ev.clientY, true, 1);
  }

  _onCanvasPointerMove(ev) {
    if (ev.pointerId !== this.menuPointerId) return;
    ev.preventDefault();
    this.getInputWriter?.()?.moveCursor(ev.clientX, ev.clientY);
  }

  _onCanvasPointerUp(ev) {
    if (ev.pointerId !== this.menuPointerId) return;
    ev.preventDefault();
    this.menuPointerId = null;
    this.getInputWriter?.()?.setCursorDown(ev.clientX, ev.clientY, false, 1);
  }

  _onWindowPointerCancel() {
    this.releaseAll();
  }

  getState() {
    return {
      isTouchDevice: this.isTouchDevice,
      mode: this.mode,
      enabled: this.isEnabled(),
      combatMode: this.combatMode,
      sensitivity: this.sensitivity,
      visible: !!(this.root && this.root.classList.contains('active')),
    };
  }
}
