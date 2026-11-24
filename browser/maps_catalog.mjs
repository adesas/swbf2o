// Complete Star Wars Battlefront II (2005) Retail Multiplayer Map, Mode & Public Lobby Catalog.
// Every single script ID here is verified present in GameData/data/_lvl_pc/mission.lvl.

export const ERA_LABELS = {
  gcw: 'Galactic Civil War',
  cw: 'Clone Wars',
};

export const MODE_LABELS = {
  con: 'Conquest',
  eli: 'Hero Assault',
  ass: 'Space Assault',
  ctf: '2-Flag CTF',
  '1flag': '1-Flag CTF',
  hunt: 'Hunt',
  xl: 'XL Battle',
};

export const MODE_SHORT = {
  con: 'Conquest',
  eli: 'Hero Assault',
  ass: 'Space Assault',
  ctf: 'CTF',
  '1flag': '1-Flag',
  hunt: 'Hunt',
  xl: 'XL',
};

// All 24 retail maps (18 ground planets + 6 space arenas) with every multiplayer script in mission.lvl.
export const MAP_CATALOG = [
  // ---- Ground Maps (18) ----
  {
    id: 'tat2', name: 'Tatooine: Mos Eisley', category: 'ground',
    scripts: [
      { id: 'tat2g_con', era: 'gcw', mode: 'con' },
      { id: 'tat2c_con', era: 'cw',  mode: 'con' },
      { id: 'tat2g_eli', era: 'gcw', mode: 'eli' },
      { id: 'tat2g_ctf', era: 'gcw', mode: 'ctf' },
      { id: 'tat2c_ctf', era: 'cw',  mode: 'ctf' },
      { id: 'tat2g_hunt', era: 'gcw', mode: 'hunt' },
    ],
  },
  {
    id: 'cor1', name: 'Coruscant: Jedi Temple', category: 'ground',
    scripts: [
      { id: 'cor1c_con', era: 'cw',  mode: 'con' },
      { id: 'cor1g_con', era: 'gcw', mode: 'con' },
      { id: 'cor1c_ctf', era: 'cw',  mode: 'ctf' },
      { id: 'cor1g_ctf', era: 'gcw', mode: 'ctf' },
    ],
  },
  {
    id: 'hot1', name: 'Hoth: Echo Base', category: 'ground',
    scripts: [
      { id: 'hot1g_con',   era: 'gcw', mode: 'con' },
      { id: 'hot1g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'hot1g_hunt',  era: 'gcw', mode: 'hunt' },
      { id: 'hot1g_xl',    era: 'gcw', mode: 'xl' },
    ],
  },
  {
    id: 'dea1', name: 'Death Star: Interior', category: 'ground',
    scripts: [
      { id: 'dea1g_con',   era: 'gcw', mode: 'con' },
      { id: 'dea1c_con',   era: 'cw',  mode: 'con' },
      { id: 'dea1g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'dea1c_1flag', era: 'cw',  mode: '1flag' },
    ],
  },
  {
    id: 'kam1', name: 'Kamino: Cloning Facility', category: 'ground',
    scripts: [
      { id: 'kam1c_con',   era: 'cw',  mode: 'con' },
      { id: 'kam1g_con',   era: 'gcw', mode: 'con' },
      { id: 'kam1c_1flag', era: 'cw',  mode: '1flag' },
      { id: 'kam1g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
  {
    id: 'mus1', name: 'Mustafar: Refinery', category: 'ground',
    scripts: [
      { id: 'mus1c_con', era: 'cw',  mode: 'con' },
      { id: 'mus1g_con', era: 'gcw', mode: 'con' },
      { id: 'mus1c_ctf', era: 'cw',  mode: 'ctf' },
      { id: 'mus1g_ctf', era: 'gcw', mode: 'ctf' },
    ],
  },
  {
    id: 'kas2', name: 'Kashyyyk: Beachhead', category: 'ground',
    scripts: [
      { id: 'kas2c_con',  era: 'cw',  mode: 'con' },
      { id: 'kas2g_con',  era: 'gcw', mode: 'con' },
      { id: 'kas2c_ctf',  era: 'cw',  mode: 'ctf' },
      { id: 'kas2g_ctf',  era: 'gcw', mode: 'ctf' },
      { id: 'kas2c_hunt', era: 'cw',  mode: 'hunt' },
      { id: 'kas2c_xl',   era: 'cw',  mode: 'xl' },
      { id: 'kas2g_xl',   era: 'gcw', mode: 'xl' },
    ],
  },
  {
    id: 'geo1', name: 'Geonosis: Dust Plains', category: 'ground',
    scripts: [
      { id: 'geo1c_con',  era: 'cw', mode: 'con' },
      { id: 'geo1c_ctf',  era: 'cw', mode: 'ctf' },
      { id: 'geo1c_hunt', era: 'cw', mode: 'hunt' },
      { id: 'geo1c_xl',   era: 'cw', mode: 'xl' },
    ],
  },
  {
    id: 'end1', name: 'Endor: Bunker', category: 'ground',
    scripts: [
      { id: 'end1g_con',   era: 'gcw', mode: 'con' },
      { id: 'end1g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'end1g_hunt',  era: 'gcw', mode: 'hunt' },
    ],
  },
  {
    id: 'nab2', name: 'Naboo: Theed', category: 'ground',
    scripts: [
      { id: 'nab2c_con',  era: 'cw',  mode: 'con' },
      { id: 'nab2g_con',  era: 'gcw', mode: 'con' },
      { id: 'nab2c_ctf',  era: 'cw',  mode: 'ctf' },
      { id: 'nab2g_ctf',  era: 'gcw', mode: 'ctf' },
      { id: 'nab2c_hunt', era: 'cw',  mode: 'hunt' },
    ],
  },
  {
    id: 'uta1', name: 'Utapau: Sinkhole', category: 'ground',
    scripts: [
      { id: 'uta1c_con',   era: 'cw',  mode: 'con' },
      { id: 'uta1g_con',   era: 'gcw', mode: 'con' },
      { id: 'uta1c_1flag', era: 'cw',  mode: '1flag' },
      { id: 'uta1g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
  {
    id: 'tan1', name: 'Tantive IV: Interior', category: 'ground',
    scripts: [
      { id: 'tan1g_con',   era: 'gcw', mode: 'con' },
      { id: 'tan1c_con',   era: 'cw',  mode: 'con' },
      { id: 'tan1g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'tan1c_1flag', era: 'cw',  mode: '1flag' },
    ],
  },
  {
    id: 'myg1', name: 'Mygeeto: War-Torn City', category: 'ground',
    scripts: [
      { id: 'myg1c_con', era: 'cw',  mode: 'con' },
      { id: 'myg1g_con', era: 'gcw', mode: 'con' },
      { id: 'myg1c_ctf', era: 'cw',  mode: 'ctf' },
      { id: 'myg1g_ctf', era: 'gcw', mode: 'ctf' },
    ],
  },
  {
    id: 'fel1', name: 'Felucia: Marshland', category: 'ground',
    scripts: [
      { id: 'fel1c_con',   era: 'cw',  mode: 'con' },
      { id: 'fel1g_con',   era: 'gcw', mode: 'con' },
      { id: 'fel1c_1flag', era: 'cw',  mode: '1flag' },
      { id: 'fel1g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
  {
    id: 'dag1', name: 'Dagobah: Swamp', category: 'ground',
    scripts: [
      { id: 'dag1g_con', era: 'gcw', mode: 'con' },
      { id: 'dag1c_con', era: 'cw',  mode: 'con' },
      { id: 'dag1g_ctf', era: 'gcw', mode: 'ctf' },
      { id: 'dag1c_ctf', era: 'cw',  mode: 'ctf' },
    ],
  },
  {
    id: 'pol1', name: 'Polis Massa: Medical Facility', category: 'ground',
    scripts: [
      { id: 'pol1c_con', era: 'cw',  mode: 'con' },
      { id: 'pol1g_con', era: 'gcw', mode: 'con' },
      { id: 'pol1c_ctf', era: 'cw',  mode: 'ctf' },
      { id: 'pol1g_ctf', era: 'gcw', mode: 'ctf' },
    ],
  },
  {
    id: 'tat3', name: "Tatooine: Jabba's Palace", category: 'ground',
    scripts: [
      { id: 'tat3g_con',   era: 'gcw', mode: 'con' },
      { id: 'tat3c_con',   era: 'cw',  mode: 'con' },
      { id: 'tat3g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'tat3c_1flag', era: 'cw',  mode: '1flag' },
    ],
  },
  {
    id: 'yav1', name: 'Yavin 4: Temple', category: 'ground',
    scripts: [
      { id: 'yav1g_con',   era: 'gcw', mode: 'con' },
      { id: 'yav1c_con',   era: 'cw',  mode: 'con' },
      { id: 'yav1g_1flag', era: 'gcw', mode: '1flag' },
      { id: 'yav1c_1flag', era: 'cw',  mode: '1flag' },
    ],
  },

  // ---- Space Maps (6) ----
  {
    id: 'spa1', name: 'Space Yavin', category: 'space',
    scripts: [
      { id: 'spa1g_ass',   era: 'gcw', mode: 'ass' },
      { id: 'spa1g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
  {
    id: 'spa3', name: 'Space Kashyyyk', category: 'space',
    scripts: [
      { id: 'spa3c_ass',   era: 'cw', mode: 'ass' },
      { id: 'spa3c_1flag', era: 'cw', mode: '1flag' },
    ],
  },
  {
    id: 'spa6', name: 'Space Mygeeto', category: 'space',
    scripts: [
      { id: 'spa6c_ass',   era: 'cw', mode: 'ass' },
      { id: 'spa6c_1flag', era: 'cw', mode: '1flag' },
    ],
  },
  {
    id: 'spa7', name: 'Space Felucia', category: 'space',
    scripts: [
      { id: 'spa7c_ass',   era: 'cw', mode: 'ass' },
      { id: 'spa7c_1flag', era: 'cw', mode: '1flag' },
    ],
  },
  {
    id: 'spa8', name: 'Space Hoth', category: 'space',
    scripts: [
      { id: 'spa8g_ass',   era: 'gcw', mode: 'ass' },
      { id: 'spa8g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
  {
    id: 'spa9', name: 'Space Tatooine', category: 'space',
    scripts: [
      { id: 'spa9g_ass',   era: 'gcw', mode: 'ass' },
      { id: 'spa9g_1flag', era: 'gcw', mode: '1flag' },
    ],
  },
];

// Flat lookup map: scriptId -> { id, mapId, mapName, category, era, eraLabel, mode, modeLabel, label }
export const SCRIPT_INFO = new Map();
export const ALL_SCRIPTS = [];
for (const m of MAP_CATALOG) {
  for (const s of m.scripts) {
    const entry = {
      id: s.id,
      mapId: m.id,
      mapName: m.name,
      category: m.category,
      era: s.era,
      eraLabel: ERA_LABELS[s.era] || s.era,
      mode: s.mode,
      modeLabel: MODE_LABELS[s.mode] || s.mode,
      label: `${m.name} · ${MODE_SHORT[s.mode] || s.mode} (${s.era === 'cw' ? 'CW' : 'GCW'})`,
    };
    SCRIPT_INFO.set(s.id, entry);
    ALL_SCRIPTS.push(entry);
  }
}

export function getScriptLabel(scriptId) {
  const info = SCRIPT_INFO.get(scriptId);
  return info ? info.label : (scriptId || 'Unknown Map');
}

export function getScriptInfo(scriptId) {
  return SCRIPT_INFO.get(scriptId) || {
    id: scriptId || 'tat2g_con',
    mapId: 'tat2',
    mapName: scriptId || 'Mos Eisley',
    category: 'ground',
    era: 'gcw',
    eraLabel: 'Galactic Civil War',
    mode: 'con',
    modeLabel: 'Conquest',
    label: scriptId || 'Mos Eisley · Conquest',
  };
}

// 16 Curated Public Lobbies spanning every iconic planet, era, and game mode (including multi-map rotations).
export const PUBLIC_LOBBY_PRESETS = [
  {
    id: 'mos_eisley_conquest',
    title: 'Mos Eisley Cantina #1',
    map: 'tat2g_con',
    playlist: ['tat2g_con', 'tat2c_con'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'heroes_vs_villains',
    title: 'Mos Eisley Hero Assault',
    map: 'tat2g_eli',
    playlist: ['tat2g_eli'],
    category: 'eli',
    rules: { maxPlayers: 16, numBots: 12, difficulty: 2, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'space_yavin_fleet',
    title: 'Space Yavin Starfighters',
    map: 'spa1g_ass',
    playlist: ['spa1g_ass', 'spa8g_ass', 'spa9g_ass'],
    category: 'space',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 0, assScore: 180 },
  },
  {
    id: 'coruscant_temple',
    title: 'Coruscant: Jedi Temple',
    map: 'cor1c_con',
    playlist: ['cor1c_con', 'cor1g_con'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 150, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'hoth_echo_base',
    title: 'Hoth: Echo Base Assault',
    map: 'hot1g_con',
    playlist: ['hot1g_con', 'hot1g_1flag'],
    category: 'con',
    rules: { maxPlayers: 32, numBots: 24, difficulty: 2, reinfMult: 150, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'hoth_xl_warfare',
    title: 'Hoth XL: Massive Battle',
    map: 'hot1g_xl',
    playlist: ['hot1g_xl', 'geo1c_xl', 'kas2c_xl', 'kas2g_xl'],
    category: 'hunt_xl',
    rules: { maxPlayers: 32, numBots: 32, difficulty: 3, reinfMult: 200, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'death_star_interior',
    title: 'Death Star: Detention Block',
    map: 'dea1g_con',
    playlist: ['dea1g_con', 'dea1c_con', 'dea1g_1flag'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'kamino_cloning',
    title: 'Kamino: Clone Wars Storm',
    map: 'kam1c_con',
    playlist: ['kam1c_con', 'kam1g_con'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'mustafar_refinery',
    title: 'Mustafar: Duel on the Lava',
    map: 'mus1c_con',
    playlist: ['mus1c_con', 'mus1g_con'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 3, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'kashyyyk_beachhead',
    title: 'Kashyyyk: Beachhead Invasion',
    map: 'kas2c_con',
    playlist: ['kas2c_con', 'kas2g_con', 'kas2c_xl'],
    category: 'con',
    rules: { maxPlayers: 32, numBots: 24, difficulty: 2, reinfMult: 150, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'geonosis_clone_wars',
    title: 'Geonosis: Clone Wars Front',
    map: 'geo1c_con',
    playlist: ['geo1c_con', 'geo1c_xl', 'geo1c_ctf'],
    category: 'con',
    rules: { maxPlayers: 32, numBots: 24, difficulty: 2, reinfMult: 150, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'tantive_boarding',
    title: 'Tantive IV: Close Quarters',
    map: 'tan1g_con',
    playlist: ['tan1g_con', 'tan1c_con', 'tan1g_1flag'],
    category: 'con',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'space_clone_wars',
    title: 'Clone Wars Fleet Armada',
    map: 'spa3c_ass',
    playlist: ['spa3c_ass', 'spa6c_ass', 'spa7c_ass'],
    category: 'space',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, reinfMult: 100, heroes: 0, assScore: 220 },
  },
  {
    id: 'ctf_classic_rotation',
    title: 'Capture The Flag Championship',
    map: 'nab2c_ctf',
    playlist: ['nab2c_ctf', 'cor1c_ctf', 'tat2g_ctf', 'mus1c_ctf', 'myg1c_ctf', 'pol1c_ctf'],
    category: 'ctf',
    rules: { maxPlayers: 16, numBots: 12, difficulty: 2, ctfScore: 5, heroes: 1, teamDamage: 0 },
  },
  {
    id: 'hunt_creature_chaos',
    title: 'Galactic Hunt: Ewoks, Wampas & Jawas',
    map: 'end1g_hunt',
    playlist: ['end1g_hunt', 'hot1g_hunt', 'tat2g_hunt', 'nab2c_hunt', 'kas2c_hunt', 'geo1c_hunt'],
    category: 'hunt_xl',
    rules: { maxPlayers: 16, numBots: 16, difficulty: 2, huntScore: 50, huntTime: 8, heroes: 0 },
  },
  {
    id: 'galactic_all_maps',
    title: 'Galactic Tour (All-Map Rotation)',
    map: 'tat2g_con',
    playlist: [
      'tat2g_con', 'cor1c_con', 'hot1g_con', 'dea1g_con', 'kam1c_con', 'mus1c_con',
      'kas2c_con', 'geo1c_con', 'end1g_con', 'nab2c_con', 'uta1c_con', 'tan1g_con',
      'myg1c_con', 'fel1c_con', 'dag1g_con', 'pol1c_con', 'tat3g_con', 'yav1g_con',
      'spa1g_ass', 'spa3c_ass',
    ],
    category: 'con',
    rules: { maxPlayers: 32, numBots: 20, difficulty: 2, reinfMult: 100, heroes: 1, randomize: 1 },
  },
];
