# Graph Execution Engine

A single-process runtime for building, validating, and executing processing graphs for media, inference, and custom data flows.

This file defines the language of the project. It contains concepts only; structures, algorithms, and interfaces live in the design documents (`02-架构设计.md`, `12-详细设计.md`, `13-接口与类设计.md`).

## Language

**Graph**:
A directed processing topology composed of Nodes, Ports, and Edges.
_Avoid_: pipeline, workflow

**Node**:
A unit of data processing, such as decode, scale, inference, mix, or encode. A Node is an instance of an Operator.
_Avoid_: stage, step, worker

**Operator**:
A reusable Node implementation identified by `type@version`, delivered by the engine or by an independent shared object.
_Avoid_: plugin (use only for the shared object that carries Operators), kernel

**Port**:
An input or output slot of a Node.
_Avoid_: pin, socket

**Edge**:
A bounded data channel between two Ports, with its own queue, backpressure policy, and synchronization semantics.
_Avoid_: link, connection, stream

**Packet**:
The unit of data flowing on an Edge: sequence number, PTS/DTS, flags, metadata, and a reference-counted payload.
_Avoid_: frame, message, event

**GraphSpec**:
The JSON intermediate representation of a Graph. Both configuration and API-driven construction converge on it.
_Avoid_: config, template, patch

**Session**:
A running instance of a Graph with isolated topology, resources, and mutation history.
_Avoid_: graph, runtime, context

**Control Plane**:
The part of the engine that validates GraphSpecs, negotiates capabilities, admits resources, warms Nodes, and publishes topology changes.
_Avoid_: runtime, scheduler, orchestration

**Data Plane**:
The part of the engine that moves Packets, applies backpressure, executes Node logic, and emits events on an already-published topology.
_Avoid_: control path, hot path

**RuntimeTopology**:
The immutable execution snapshot that the Data Plane consumes after a Mutation is published. Identified by a topology version.
_Avoid_: graph spec, patch, snapshot (alone)

**Mutation**:
A transaction-like change to a running Graph's topology that is validated, negotiated, admitted, warmed, and published atomically; either fully applied or fully rejected.
_Avoid_: patch, update, edit

**Parameter Update**:
A change to one Node's parameters that takes effect from that Node's next input Packet without changing topology.
_Avoid_: mutation, hot reload, config change

**CapabilityDescriptor**:
The machine-readable declaration of what an Operator can consume, emit, and execute, including port contracts, media/tensor formats, execution resources, and functional metadata.
_Avoid_: config, manifest, schema

**ConnectionContract**:
The negotiated, frozen data contract of one Edge: logical type, format, memory placement, device, synchronization semantics, and capability versions.
_Avoid_: capability, binding, format

**Completion Queue**:
The engine-owned queue through which asynchronous Operators deliver completion events; the engine consumes, validates, and dispatches them.
_Avoid_: callback, future, promise

**Rendition**:
One transcoded output variant derived from the same input source.
_Avoid_: profile, branch (alone), quality

**Fan-out**:
One output Port feeding multiple downstream input Ports with a single shared ConnectionContract and zero-copy payload sharing.
_Avoid_: broadcast, tee (Tee is a Node category, not the concept)

**End of Stream**:
The signal that no further Packets will arrive on a Port; propagates along the Graph so Sinks close after receiving all data.
_Avoid_: close, stop, finish

## Relationships

- A **Graph** is described by exactly one **GraphSpec**; many **Session**s may run from the same GraphSpec.
- A **Session** owns one active **RuntimeTopology** at a time and may retain retired ones until they drain.
- A **Node** is created from an **Operator** and exposes **Port**s; an **Edge** connects one output Port to one input Port.
- A **ConnectionContract** is derived from two **CapabilityDescriptor**s at build, validate, create, or Mutation time; it never changes while its RuntimeTopology is active.
- A **Fan-out** shares one ConnectionContract across all downstream Edges; the contract must lie in the intersection of every downstream capability.
- A **Mutation** produces a candidate RuntimeTopology and replaces the active one only after every stage succeeds; a **Parameter Update** never replaces the RuntimeTopology.
- The **Control Plane** publishes RuntimeTopologies; the **Data Plane** consumes them and never modifies them.
- Asynchronous Operators report results only through the **Completion Queue**; results that belong to a retired RuntimeTopology are never injected into the active one.
- **End of Stream** propagates through the Data Plane and is not blocked by observation or control traffic.

## Example dialogue

> **Architect:** "When a **Mutation** adds a **Rendition**, do we replace the active **RuntimeTopology** immediately?"
> **Engineer:** "No — the **Control Plane** validates, negotiates every new **ConnectionContract**, admits resources, and warms the new **Node**s first; then it publishes atomically so the **Data Plane** never sees a half-applied graph."

> **Architect:** "Can the encoder's bitrate change be a Mutation?"
> **Engineer:** "No — that is a **Parameter Update**; it takes effect from the next **Packet** and keeps the same RuntimeTopology."

## Flagged ambiguities

- "update" was used for both parameter changes and topology changes — resolved: **Mutation** covers topology; **Parameter Update** covers parameters.
- "runtime" was used for the engine, a Session, and a Node instance — resolved: use **Data Plane** for the engine part, **Session** for the running graph, **Node** for the instance.
- "plugin" was used for both a shared object and an Operator — resolved: **Operator** is the unit of processing; "plugin" refers only to the shared object that carries Operators.
- "capability" was used for both the declaration and the negotiated result — resolved: **CapabilityDescriptor** is declared; **ConnectionContract** is negotiated.
