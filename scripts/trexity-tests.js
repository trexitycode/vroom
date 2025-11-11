#!/usr/bin/env node

/*
 Simple Node.js test runner for VROOM CLI.
 Mirrors tests from scripts/trexity-tests.sh including pinned semantics.
*/

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

function findBinary() {
  const envBin = process.env.VROOM_BIN;
  if (envBin && fs.existsSync(envBin)) return envBin;
  const root = path.resolve(__dirname, '..');
  const macos = path.join(root, 'bin', 'vroom-macos');
  const linux = path.join(root, 'bin', 'vroom');
  if (fs.existsSync(macos)) return macos;
  if (fs.existsSync(linux)) return linux;
  throw new Error('FATAL: No vroom binary found. Set VROOM_BIN or build first.');
}

const BIN = findBinary();

function runVroom(inputPath) {
  const res = spawnSync(BIN, ['-i', inputPath], { encoding: 'utf8' });
  const stdout = res.stdout || '';
  const code = res.status ?? 1;
  let json;
  try {
    json = JSON.parse(stdout || '{}');
  } catch (_) {
    json = {};
  }
  return { code, json, stdout };
}

function tmpDir() {
  // Ensure base tmp exists even if process.env.TMPDIR points to a removed dir
  let base = os.tmpdir();
  try {
    if (!fs.existsSync(base)) {
      fs.mkdirSync(base, { recursive: true });
    }
  } catch (_) {
    base = '/tmp';
  }
  if (!fs.existsSync(base)) base = '/tmp';
  return fs.mkdtempSync(path.join(base, 'trexity-vroom-node-tests.'));
}

function writeJSON(tmp, name, obj) {
  const p = path.join(tmp, name);
  fs.writeFileSync(p, JSON.stringify(obj));
  return p;
}

function matrix2() {
  return { car: { durations: [[0, 1000], [1000, 0]] } };
}
function matrix2_500() {
  return { car: { durations: [[0, 500], [500, 0]] } };
}
function matrix3_200() {
  return { car: { durations: [[0, 200, 200], [200, 0, 200], [200, 200, 0]] } };
}

// Skewed 3x3 matrix to make the optimal order Start -> Job2 -> Job1
// cheaper than Start -> Job1 -> Job2.
function matrix3_skewed() {
  // indices: 0=start, 1=job1, 2=job2
  // 0->2 and 2->1 are cheap; 0->1 is very expensive, so solver should place job2 before job1
  return { car: { durations: [[0, 1000, 100], [1000, 0, 100], [100, 100, 0]] } };
}

// Make job2 clearly cheaper (lower travel cost) while both single-job routes are feasible
// indices: 0=start, 1=job1, 2=job2
// 0->1=300, 0->2=100, 2->1=200, 1->2=200
function matrix3_cheaper_job2() {
  return { car: { durations: [[0, 300, 100], [300, 0, 200], [100, 200, 0]] } };
}

function matrix2_100() {
  return { car: { durations: [[0, 100], [100, 0]] } };
}

function matrix3_chain_50() {
  // 0->1=50, 1->2=50, 0->2=999 (not used)
  return { car: { durations: [[0, 50, 999], [50, 0, 50], [999, 50, 0]] } };
}

function matrix3_chain_50_both() {
  const durations = [[0, 50, 999], [50, 0, 50], [999, 50, 0]];
  const distances = [[0, 500, 9990], [500, 0, 500], [9990, 500, 0]];
  return { car: { durations, distances } };
}

function assertExit(exp, got) {
  if (exp !== got) throw new Error(`Expected exit ${exp}, got ${got}`);
}
function assertJsonEq(obj, jqPath, expected) {
  const parts = jqPath.replace(/^\./, '').split('.');
  let cur = obj;
  for (const p of parts) {
    if (p === '') continue;
    if (!(p in cur)) throw new Error(`JSON path not found: ${jqPath}`);
    cur = cur[p];
  }
  if (String(cur) !== String(expected)) {
    throw new Error(`Assert ${jqPath} == ${expected} failed (got ${cur})`);
  }
}

async function run(name, fn) {
  process.stdout.write(`[TEST] ${name}\n`);
  try {
    await fn();
    process.stdout.write('  PASS\n');
    return true;
  } catch (e) {
    process.stdout.write(`  FAIL: ${e.message}\n`);
    return false;
  }
}

// ----------------------- Tests -----------------------

const tests = {
  async budget_single_ok() {
    const t = tmpDir();
    const input = {
      include_action_time_in_budget: true,
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, budget: 100 }], // travel 100s -> cost 100
      matrices: matrix2_100()
    };
    const f = writeJSON(t, 'budget_single_ok.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async budget_single_insufficient() {
    const t = tmpDir();
    const input = {
      include_action_time_in_budget: true,
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, budget: 99 }], // need 100
      matrices: matrix2_100()
    };
    const f = writeJSON(t, 'budget_single_insufficient.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 1);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async budget_shipment_ok() {
    const t = tmpDir();
    const input = {
      include_action_time_in_budget: true,
      vehicles: [{
        id: 101,
        start_index: 0,
        capacity: [1],
        steps: [
          { type: 'start' },
          { type: 'pickup', id: 11 },
          { type: 'delivery', id: 12 },
          { type: 'end' }
        ]
      }],
      shipments: [{
        amount: [1],
        budget: 101,
        pickup: { id: 11, location_index: 1 },
        delivery: { id: 12, location_index: 2 }
      }],
      matrices: matrix3_chain_50()
    };
    const f = writeJSON(t, 'budget_shipment_ok.json', input);
    const { code, json, stdout } = runVroom(f);
    if (code !== 0) {
      process.stdout.write(`  DEBUG budget_shipment_ok stdout:\n${stdout}\n`);
    }
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async budget_counts_service_and_setup() {
    const t = tmpDir();
    const input = {
      include_action_time_in_budget: true,
      vehicles: [{ id: 101, start_index: 0 }],
      // travel 50 + service 30 => 80 budget required
      jobs: [{ id: 1, location_index: 1, service: 30, budget: 79 }],
      matrices: { car: { durations: [[0, 50], [50, 0]] } }
    };
    // First, insufficient
    let f = writeJSON(t, 'budget_counts_action_insufficient.json', input);
    let r = runVroom(f);
    assertExit(0, r.code);
    assertJsonEq(r.json, '.summary.unassigned', 1);
    // Then sufficient
    input.jobs[0].budget = 80;
    f = writeJSON(t, 'budget_counts_action_sufficient.json', input);
    r = runVroom(f);
    assertExit(0, r.code);
    assertJsonEq(r.json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },
  async job_allowed_unassigned() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, allowed_vehicles: [999] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'job_allowed_unassigned.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 1);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async job_pinned_allowed_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'job_pinned_allowed_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async shipment_allowed_unassigned() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1] }],
      shipments: [{ amount: [1], allowed_vehicles: [999],
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'shipment_allowed_unassigned.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 2);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async shipment_pinned_allowed_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, allowed_vehicles: [101],
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'shipment_pinned_allowed_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async skills_and_allowed_ok() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, skills: [1], steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, skills: [1], pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'skills_allowed_ok.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async skills_and_allowed_fail() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, skills: [1], steps: [
        { type: 'start' }, { type: 'job', id: 2 }, { type: 'end' }
      ] }],
      jobs: [{ id: 2, location_index: 1, skills: [2], pinned: true, allowed_vehicles: [101] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'skills_allowed_fail.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_stays_same_vehicle() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] },
        { id: 102, start_index: 0 }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true }, { id: 2, location_index: 2 } ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_stays_same_vehicle.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_missing_in_steps_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0 }],
      jobs: [{ id: 1, location_index: 1, pinned: true }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_missing_in_steps_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_in_two_vehicles_err() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] },
        { id: 102, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true } ],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_in_two_vehicles_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_allowed_conflict_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, allowed_vehicles: [999] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_job_allowed_conflict_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_same_vehicle() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_same_vehicle.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_split_err() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, capacity: [1], steps: [ { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'end' } ] },
        { id: 102, start_index: 0, capacity: [1], steps: [ { type: 'start' }, { type: 'delivery', id: 9002 }, { type: 'end' } ] }
      ],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_split_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_infeasible_capacity_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [0], steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [{ id: 1, location_index: 1, pinned: true, pickup: [1] }],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_infeasible_capacity_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  // ---------------- Additional Edge Case Tests ----------------
  async pinned_job_reorder_with_added_task_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2 }
      ],
      matrices: matrix3_skewed()
    };
    const f = writeJSON(t, 'pinned_job_reorder_with_added_task_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    // Expect the solver to reorder due to strong skew: Job2 before Job1
    const steps = json.routes[0].steps;
    const jobSteps = steps.filter(s => s.type === 'job');
    if (jobSteps.length < 2) throw new Error('Expected 2 job steps in route');
    if (jobSteps[0].id !== 2 || jobSteps[1].id !== 1) {
      throw new Error(`Expected order [2,1], got [${jobSteps.map(s => s.id)}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_vs_unpinned_cheaper_selection() {
    const t = tmpDir();
    // Two vehicles, one job. Steps seed job 1 on vehicle 101.
    // Matrix favors vehicle 102 -> job (much cheaper). Only difference between runs is pinned flag.
    const base = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' } ] },
        { id: 102, start_index: 1 }
      ],
      jobs: [ { id: 1, location_index: 2 } ],
      matrices: {
        car: {
          durations: [
            // 0 (v101 start)  1 (v102 start)  2 (job)
            [ 0,               0,              1000 ],
            [ 0,               0,              100  ],
            [ 1000,           100,              0   ]
          ]
        }
      }
    };

    // With pinned=true, job must remain on vehicle 101 as seeded
    const withPinned = JSON.parse(JSON.stringify(base));
    withPinned.jobs[0].pinned = true;
    const f1 = writeJSON(t, 'pinned_vs_unpinned_1.json', withPinned);
    const r1 = runVroom(f1);
    assertExit(0, r1.code);
    // Find the route containing job 1
    const routeJob1Pinned = r1.json.routes.find(rt => rt.steps.some(s => s.type === 'job' && s.id === 1));
    if (!routeJob1Pinned || routeJob1Pinned.vehicle !== 101) {
      throw new Error(`Pinned run: expected job 1 on vehicle 101, got ${routeJob1Pinned && routeJob1Pinned.vehicle}`);
    }

    // With pinned=false, optimizer should migrate job 1 to cheaper vehicle 102
    const withoutPinned = JSON.parse(JSON.stringify(base));
    withoutPinned.jobs[0].pinned = false;
    const f2 = writeJSON(t, 'pinned_vs_unpinned_2.json', withoutPinned);
    const r2 = runVroom(f2);
    assertExit(0, r2.code);
    const routeJob1Unpinned = r2.json.routes.find(rt => rt.steps.some(s => s.type === 'job' && s.id === 1));
    if (!routeJob1Unpinned || routeJob1Unpinned.vehicle !== 102) {
      throw new Error(`Unpinned run: expected job 1 on vehicle 102, got ${routeJob1Unpinned && routeJob1Unpinned.vehicle}`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_two_jobs_same_vehicle_success() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [ { type: 'start' }, { type: 'job', id: 1 }, { type: 'job', id: 2 }, { type: 'end' } ] },
        { id: 102, start_index: 0 }
      ],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2, pinned: true }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_two_jobs_same_vehicle_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    const steps = json.routes[0].steps;
    const jobIds = steps.filter(s => s.type === 'job').map(s => s.id).sort((a,b)=>a-b);
    if (String(jobIds) !== String([1,2])) {
      throw new Error(`Expected job steps [1,2] on vehicle 101, got [${jobIds}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_partial_steps_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true,
        pickup: { id: 9001, location_index: 0 },
        delivery: { id: 9002, location_index: 1 }
      }],
      matrices: matrix2_500()
    };
    const f = writeJSON(t, 'pinned_shipment_partial_steps_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_plus_unpinned_assigned_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true },
        { id: 2, location_index: 2 }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_plus_unpinned_assigned_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 0);
    assertJsonEq(json, '.routes.length', 1);
    assertJsonEq(json, '.routes.0.vehicle', 101);
    const steps = json.routes[0].steps;
    const jobIds = steps.filter(s => s.type === 'job').map(s => s.id).sort((a,b)=>a-b);
    if (String(jobIds) !== String([1,2])) {
      throw new Error(`Expected both jobs [1,2] on vehicle 101, got [${jobIds}]`);
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  // ---------------- pinned_position tests ----------------
  async pinned_job_first_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true, pinned_position: 'first' },
        { id: 2, location_index: 2 }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_first_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    const steps = json.routes[0].steps.filter(s => s.type === 'job');
    if (steps[0].id !== 1) throw new Error('Job 1 should be first');
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_job_last_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 2, pinned: true, pinned_position: 'last' },
        { id: 2, location_index: 1 }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_job_last_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    const steps = json.routes[0].steps.filter(s => s.type === 'job');
    if (steps[steps.length - 1].id !== 1) throw new Error('Job 1 should be last');
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_first_contiguous_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, pinned_position: 'first',
        pickup: { id: 9001, location_index: 1 },
        delivery: { id: 9002, location_index: 2 }
      }],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_shipment_first_contiguous_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    const steps = json.routes[0].steps.filter(s => s.type === 'pickup' || s.type === 'delivery');
    if (!(steps[0].type === 'pickup' && steps[0].id === 9001 && steps[1].type === 'delivery' && steps[1].id === 9002)) {
      throw new Error('Shipment should be contiguous at start');
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_last_contiguous_success() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, pinned_position: 'last',
        pickup: { id: 9001, location_index: 1 },
        delivery: { id: 9002, location_index: 2 }
      }],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_shipment_last_contiguous_success.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    const steps = json.routes[0].steps.filter(s => s.type === 'pickup' || s.type === 'delivery');
    const n = steps.length;
    if (!(steps[n-2].type === 'pickup' && steps[n-2].id === 9001 && steps[n-1].type === 'delivery' && steps[n-1].id === 9002)) {
      throw new Error('Shipment should be contiguous at end');
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_position_without_pinned_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [ { id: 1, location_index: 1, pinned_position: 'first' } ],
      matrices: matrix2()
    };
    const f = writeJSON(t, 'pinned_position_without_pinned_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  // Bias so that without anchor, optimizer puts unpinned job at end (making pinned job not last)
  async pinned_job_last_discriminating() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true, pinned_position: 'last' },
        { id: 2, location_index: 2 }
      ],
      matrices: { car: { durations: [
        // 0 start, 1 job1, 2 job2
        [0, 200, 10],
        [200, 0, 10],
        [10, 10, 0]
      ]}}
    };
    const f = writeJSON(t, 'pinned_job_last_discriminating.json', input);
    const { code, json } = runVroom(f);
    assertExit(0, code);
    const jobSteps = json.routes[0].steps.filter(s => s.type === 'job');
    if (jobSteps[jobSteps.length - 1].id !== 1) {
      throw new Error('Pinned job 1 must be last with anchor');
    }
    fs.rmSync(t, { recursive: true, force: true });
  },

  // Shipment first under pressure: extra job that would like to be first
  async pinned_shipment_first_under_pressure() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, pinned_position: 'first',
        pickup: { id: 9001, location_index: 1 },
        delivery: { id: 9002, location_index: 2 }
      }],
      jobs: [ { id: 3, location_index: 3 } ],
      matrices: { car: { durations: [
        // 0 start, 1 pickup, 2 delivery, 3 job3
        [0, 5, 5, 1],
        [5, 0, 5, 5],
        [5, 5, 0, 5],
        [1, 5, 5, 0]
      ]}}
    };
    const f = writeJSON(t, 'pinned_shipment_first_under_pressure.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  // Conflicts and mismatched shipment positions
  async pinned_position_conflict_same_vehicle_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, steps: [
        { type: 'start' }, { type: 'job', id: 1 }, { type: 'job', id: 2 }, { type: 'end' }
      ] }],
      jobs: [
        { id: 1, location_index: 1, pinned: true, pinned_position: 'first' },
        { id: 2, location_index: 2, pinned: true, pinned_position: 'first' }
      ],
      matrices: matrix3_200()
    };
    const f = writeJSON(t, 'pinned_position_conflict_same_vehicle_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  async pinned_shipment_conflict_with_job_first_err() {
    const t = tmpDir();
    const input = {
      vehicles: [{ id: 101, start_index: 0, capacity: [1], steps: [
        { type: 'start' }, { type: 'pickup', id: 9001 }, { type: 'delivery', id: 9002 }, { type: 'job', id: 1 }, { type: 'end' }
      ] }],
      shipments: [{ amount: [1], pinned: true, pinned_position: 'first',
        pickup: { id: 9001, location_index: 1 },
        delivery: { id: 9002, location_index: 2 }
      }],
      jobs: [ { id: 1, location_index: 0, pinned: true, pinned_position: 'first' } ],
      matrices: { car: { durations: [[0,10,10,10],[10,0,10,10],[10,10,0,10],[10,10,10,0]] } }
    };
    const f = writeJSON(t, 'pinned_shipment_conflict_with_job_first_err.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

// ---------------- pinned_soft_timing tests ----------------
  // With pinned_soft_timing=true and budget=0, inserting extra work before a pinned step
  // should be blocked (no additional delay allowed). We expect unassigned=1.
  async pinned_soft_timing_blocks_pre_insertion_budget0() {
    const t = tmpDir();
    const input = {
      pinned_soft_timing: true,
      pinned_lateness_limit_sec: 0,
      vehicles: [
        { id: 101, start_index: 0, capacity: [1], steps: [
          { type: 'start' },
          { type: 'pickup', id: 9001 },
          { type: 'delivery', id: 9002 },
          { type: 'end' }
        ] }
      ],
      shipments: [
        { amount: [1], pinned: true, allowed_vehicles: [101],
          pickup: { id: 9001, location_index: 1, time_windows: [[0, 1000]], service: 0 },
          delivery: { id: 9002, location_index: 2, time_windows: [[0, 5000]], service: 0 }
        }
      ],
      jobs: [ { id: 3, location_index: 3, service: 0, time_windows: [[0, 2]] } ],
      matrices: {
        car: { durations: [
          // 0 start, 1 pickup, 2 delivery, 3 extra job
          [0, 5, 5, 1],
          [5, 0, 5, 5],
          [5, 5, 0, 5],
          [3, 5, 5, 0]
        ]}
      }
    };
    const f = writeJSON(t, 'pinned_soft_timing_blocks_pre_insertion_budget0.json', input);
    const { code, json } = runVroom(f);
    // EXPECTED AFTER IMPLEMENTATION: exit 0 with extra job unassigned
    assertExit(0, code);
    assertJsonEq(json, '.summary.unassigned', 1);
    fs.rmSync(t, { recursive: true, force: true });
  },

  // With pinned_soft_timing=true and small budget, allow small added delay before pinned step
  // We expect the extra job to be assigned when budget >= delta.
  async pinned_violation_budget_allows_small_delay() {
    const t = tmpDir();
    const base = {
      vehicles: [
        { id: 101, start_index: 0, capacity: [1], steps: [
          { type: 'start' },
          { type: 'pickup', id: 9001 },
          { type: 'delivery', id: 9002 },
          { type: 'end' }
        ] }
      ],
      shipments: [
        { amount: [1], pinned: true, allowed_vehicles: [101],
          pickup: { id: 9001, location_index: 1, time_windows: [[0, 1000]], service: 0 },
          delivery: { id: 9002, location_index: 2, time_windows: [[0, 5000]], service: 0 }
        }
      ],
      jobs: [ { id: 3, location_index: 3, service: 0, time_windows: [[0, 2]] } ],
      matrices: {
        car: { durations: [
          // 0 start, 1 pickup, 2 delivery, 3 extra job
          // Going 0->3->1 adds +1s vs 0->1 direct
          [0, 5, 5, 1],
          [5, 0, 5, 5],
          [5, 5, 0, 5],
          [3, 5, 5, 0]
        ]}
      }
    };

    // Case A: budget too small (0) -> expect extra job unassigned
    const a = JSON.parse(JSON.stringify(base));
    a.pinned_soft_timing = true;
    a.pinned_lateness_limit_sec = 0;
    let f = writeJSON(t, 'pinned_budget_small.json', a);
    let r = runVroom(f);
    // EXPECTED AFTER IMPLEMENTATION: exit 0 and unassigned=1
    assertExit(0, r.code);
    assertJsonEq(r.json, '.summary.unassigned', 1);

    // Case B: budget sufficient (>=1) -> expect extra job assigned
    const b = JSON.parse(JSON.stringify(base));
    b.pinned_soft_timing = true;
    b.pinned_lateness_limit_sec = 5;
    f = writeJSON(t, 'pinned_budget_large.json', b);
    r = runVroom(f);
    // EXPECTED AFTER IMPLEMENTATION: exit 0 and unassigned=0
    assertExit(0, r.code);
    assertJsonEq(r.json, '.summary.unassigned', 0);
    const steps = r.json.routes[0].steps;
    const found = steps.some(s => s.type === 'job' && s.id === 3);
    if (!found) throw new Error('Expected extra job 3 assigned on vehicle 101');

    fs.rmSync(t, { recursive: true, force: true });
  },

  // Minimal infeasible seed: pinned job with unreachable TW under provided matrix
  // With pinned_soft_timing=true, solver should not fail; current behavior fails.
  async pinned_soft_timing_saves_infeasible_seed() {
    const t = tmpDir();
    const input = {
      pinned_soft_timing: true,
      pinned_lateness_limit_sec: 0,
      vehicles: [
        { id: 101, start_index: 0, steps: [
          { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
        ] }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true, service: 0, time_windows: [[0, 10]] } ],
      matrices: { car: { durations: [
        // 0 start, 1 job; travel = 20 > latest 10
        [0, 20],
        [20, 0]
      ] } }
    };
    const f = writeJSON(t, 'pinned_soft_timing_saves_infeasible_seed.json', input);
    const { code } = runVroom(f);
    assertExit(0, code);
    fs.rmSync(t, { recursive: true, force: true });
  },

  // Control: with pinned_soft_timing=false, infeasible seed should fail (current behavior)
  async pinned_soft_timing_off_infeasible_seed_fails() {
    const t = tmpDir();
    const input = {
      vehicles: [
        { id: 101, start_index: 0, steps: [
          { type: 'start' }, { type: 'job', id: 1 }, { type: 'end' }
        ] }
      ],
      jobs: [ { id: 1, location_index: 1, pinned: true, service: 0, time_windows: [[0, 10]] } ],
      matrices: { car: { durations: [
        [0, 20],
        [20, 0]
      ] } }
    };
    const f = writeJSON(t, 'pinned_soft_timing_off_infeasible_seed_fails.json', input);
    const { code } = runVroom(f);
    assertExit(2, code);
    fs.rmSync(t, { recursive: true, force: true });
  }
};

async function main() {
  const order = [
    'budget_single_ok',
    'budget_single_insufficient',
    'budget_shipment_ok',
    'budget_counts_service_and_setup',
    'job_allowed_unassigned',
    'job_pinned_allowed_success',
    'shipment_allowed_unassigned',
    'shipment_pinned_allowed_success',
    'skills_and_allowed_ok',
    'skills_and_allowed_fail',
    'pinned_job_stays_same_vehicle',
    'pinned_job_missing_in_steps_err',
    'pinned_job_in_two_vehicles_err',
    'pinned_job_allowed_conflict_err',
    'pinned_shipment_same_vehicle',
    'pinned_shipment_split_err',
    'pinned_infeasible_capacity_err',
    // Additional edge cases
    'pinned_job_reorder_with_added_task_success',
    'pinned_two_jobs_same_vehicle_success',
    'pinned_shipment_partial_steps_err',
    'pinned_job_plus_unpinned_assigned_success',
    'pinned_vs_unpinned_cheaper_selection',
    // pinned_position
    'pinned_job_first_success',
    'pinned_job_last_success',
    'pinned_shipment_first_contiguous_success',
    'pinned_shipment_last_contiguous_success',
    'pinned_position_without_pinned_err',
    'pinned_job_last_discriminating',
    'pinned_shipment_first_under_pressure',
    'pinned_position_conflict_same_vehicle_err',
    'pinned_shipment_conflict_with_job_first_err',
    // New tests for pinned_soft_timing semantics
    'pinned_soft_timing_blocks_pre_insertion_budget0',
    'pinned_violation_budget_allows_small_delay',
    'pinned_soft_timing_saves_infeasible_seed',
    'pinned_soft_timing_off_infeasible_seed_fails'
  ];

  let pass = 0, fail = 0;
  for (const name of order) {
    // eslint-disable-next-line no-await-in-loop
    const ok = await run(name, tests[name]);
    if (ok) pass++; else fail++;
  }
  process.stdout.write(`\nSummary: ${pass} passed, ${fail} failed\n`);
  process.exit(fail === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});


