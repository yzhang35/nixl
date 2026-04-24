## FlagCX Backend Plugin [Preview]

[FlagCX](https://github.com/FlagOpen/FlagCX) is a heterogeneous communication library that supports GPU memory transfers via one-sided RDMA operations. This backend plugin wraps the FlagCX P2P engine, mirroring the architecture of the UCCL backend plugin. The FlagCX P2P engine provides a NIXL-friendly API for point-to-point RDMA READ/WRITE operations with automatic NIC selection based on PCIe topology.

## Capabilities

Currently, the FlagCX P2P backend supports internode communication over RDMA. Intranode IPC communication will be added soon.

## Installation Guide

1. Build and install the FlagCX P2P engine library (`libflagcx_p2p`).

2. Build NIXL using regular method as in [README](https://github.com/ai-dynamo/nixl/blob/main/README.md). The FlagCX plugin is auto-discovered when `libflagcx_p2p` is found.

## Usage Guide

Example Usage to create a NIXL agent with FlagCX P2P engine:

    ```python
    config = nixl_agent_config(backends=["FLAGCX"])
    agent = nixl_agent("agent-name", config)
    ```

The FlagCX engine auto-discovers the right NIC based on PCIe topology during memory registration.

### Road Map

- Intra-node IPC communication support
- Progress thread support
