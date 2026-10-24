/** One traversal of one residual arc inside an augmentation. */
export interface Step {
  edge: number;
  from: string | number;
  to: string | number;
  direction: "forward" | "backward";
  multiplier: number;
  flow_in: number;
  flow_out: number;
}

/** A filled path, or a flow-generating cycle and the route carrying its excess. */
export interface FilledPath {
  kind: "path" | "flow_generating_cycle";
  flow_in: number;
  flow_out: number;
  gain: number;
  nodes: Array<string | number>;
  steps: Step[];
  /** The constricting edge: the one that ran out and limited this path. */
  bottleneck?: {
    edge: number;
    from: string | number;
    to: string | number;
    direction: "forward" | "backward";
    step: number;
  };
  /** Cycle entries only: how many steps belong to the cycle. */
  cycle_length?: number;
  /** Cycle entries only: the flow the cycle manufactured. */
  excess?: number;
}

export interface EdgeFlow {
  index: number;
  from: string | number;
  to: string | number;
  capacity: number;
  multiplier: number;
  flow: number;
  delivered: number;
}

/** The proof that the answer is maximum. Both flags true means optimal. */
export interface Certificate {
  no_augmenting_path: boolean;
  no_generating_cycle_reaching_sink: boolean;
  max_conservation_error: number;
  max_capacity_violation: number;
}

export interface Stats {
  iterations: number;
  path_searches: number;
  cycle_searches: number;
  relaxations: number;
  parallel_fallbacks: number;
  threads_used: number;
}

export type Status =
  | "optimal"
  | "unbounded"
  | "iteration_limit"
  | "suboptimal"
  | "generating_cycle_rejected"
  | "invalid_input";

export interface Answer {
  status: Status;
  optimal: boolean;
  message?: string;
  /** Delivered at the sink. */
  max_flow: number;
  /** Injected at the source; differs from max_flow on a lossy network. */
  source_outflow: number;
  source: string | number;
  sink: string | number;
  num_nodes: number;
  paths: FilledPath[];
  edges: EdgeFlow[];
  certificate: Certificate;
  stats: Stats;
  version: string;
}

export interface EdgeInput {
  from?: string | number;
  to: string | number;
  /** Omitted or null means uncapacitated. */
  capacity?: number | null;
  /** Units delivered per unit entering. Defaults to 1. */
  multiplier?: number;
}

export interface Options {
  epsilon?: number;
  /** 1 = sequential, 0 = every core. */
  threads?: number;
  max_iterations?: number;
  collect_paths?: boolean;
  cancel_generating_cycles?: boolean;
  pretty?: boolean;
}

export interface Graph {
  source: string | number;
  sink: string | number;
  nodes?: Array<string | number>;
  adjacency?: Record<string, EdgeInput[]> | EdgeInput[][];
  edges?: EdgeInput[];
  options?: Options;
}

export interface Solver {
  /** Solves a graph object (or JSON text) and returns the answer object. */
  solve(graph: Graph | string): Answer;
  /** Solves JSON text and returns JSON text, with no parsing in between. */
  solveText(request: string): string;
  /** The flow delivered at the sink; Infinity when unbounded. */
  maxFlow(graph: Graph | string): number;
  version(): string;
  /** The underlying Emscripten runtime. */
  runtime: unknown;
}

/** Loads the WebAssembly module and returns a solver bound to it. */
export default function createSolver(moduleOptions?: Record<string, unknown>): Promise<Solver>;
