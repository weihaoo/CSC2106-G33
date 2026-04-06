#!/usr/bin/env python3
"""
LoRa Mesh Metrics Analysis Script
CSC2106 Group 33

Parses serial logs from edge node and generates metrics graphs for the report.
Supports both captured logs (via serial_capture.py) and copy-pasted text.

Usage:
    python metrics_analysis.py logs/test_run.log --output report/
    python metrics_analysis.py logs/*.log --compare --output comparison/

Dependencies:
    pip install matplotlib pandas

Output:
    - summary.csv: Table of all metrics
    - latency_histogram.png: Latency distribution
    - latency_timeline.png: Latency over time
    - pdr_by_node.png: Per-node PDR bar chart
    - hop_distribution.png: Hop count histogram
    - throughput_timeline.png: Throughput over time
    - packet_loss_cumulative.png: Cumulative packet loss
"""

import argparse
import re
import sys
from pathlib import Path
from collections import defaultdict
from datetime import datetime

try:
    import matplotlib.pyplot as plt
    import pandas as pd
except ImportError:
    print("ERROR: Missing dependencies. Run: pip install matplotlib pandas")
    sys.exit(1)


# ── Regex patterns for parsing serial logs ──
PATTERNS = {
    # RX_MESH | src=0x03 | seq=42 | hops=2 | rssi=-85
    'rx_mesh': re.compile(
        r'RX_MESH\s*\|\s*src=0x([0-9A-Fa-f]+)\s*\|\s*seq=(\d+)\s*\|\s*hops=(\d+)\s*\|\s*rssi=(-?\d+)'
    ),
    # LATENCY | src=0x03 | send_ts=... | recv_ts=... | one-way ~123 ms
    'latency': re.compile(
        r'LATENCY\s*\|\s*src=0x([0-9A-Fa-f]+).*one-way\s*~?\s*(\d+)\s*ms'
    ),
    # [DROP] CRC_FAIL | src=0x03 seq=42  OR  [DROP] Duplicate | src=0x03 seq=42
    'drop': re.compile(
        r'\[DROP\].*\|\s*src=0x([0-9A-Fa-f]+)\s*seq=(\d+)'
    ),
    # Timestamp at start of line: [HH:MM:SS.mmm]
    'timestamp': re.compile(
        r'^\[(\d{2}:\d{2}:\d{2}\.\d{3})\]'
    )
}


class MetricsParser:
    """Parses serial log files and extracts metrics."""
    
    def __init__(self):
        self.packets = []  # List of {timestamp, src_id, seq, hops, rssi, latency_ms}
        self.drops = []    # List of {timestamp, src_id, seq}
        
    def parse_file(self, filepath):
        """Parse a log file and extract metrics."""
        with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
        
        current_packet = {}
        line_num = 0
        
        for line in lines:
            line_num += 1
            
            # Skip comment lines
            if line.strip().startswith('#'):
                continue
            
            # Try to extract timestamp
            ts_match = PATTERNS['timestamp'].match(line)
            timestamp = ts_match.group(1) if ts_match else None
            
            # Check for RX_MESH line
            rx_match = PATTERNS['rx_mesh'].search(line)
            if rx_match:
                current_packet = {
                    'timestamp': timestamp,
                    'line_num': line_num,
                    'src_id': rx_match.group(1).upper(),
                    'seq': int(rx_match.group(2)),
                    'hops': int(rx_match.group(3)),
                    'rssi': int(rx_match.group(4)),
                    'latency_ms': None
                }
                continue
            
            # Check for LATENCY line (follows RX_MESH)
            lat_match = PATTERNS['latency'].search(line)
            if lat_match and current_packet:
                src_id = lat_match.group(1).upper()
                if current_packet.get('src_id') == src_id:
                    current_packet['latency_ms'] = int(lat_match.group(2))
                    self.packets.append(current_packet.copy())
                    current_packet = {}
                continue
            
            # Check for DROP line
            drop_match = PATTERNS['drop'].search(line)
            if drop_match:
                self.drops.append({
                    'timestamp': timestamp,
                    'line_num': line_num,
                    'src_id': drop_match.group(1).upper(),
                    'seq': int(drop_match.group(2))
                })
                continue
            
            # If we have a packet without latency, save it anyway
            if current_packet and current_packet.get('src_id'):
                # Check if this line has no metrics data
                if not any(p.search(line) for p in PATTERNS.values()):
                    self.packets.append(current_packet.copy())
                    current_packet = {}
        
        # Don't forget last packet
        if current_packet and current_packet.get('src_id'):
            self.packets.append(current_packet)
        
        print(f"Parsed {filepath}: {len(self.packets)} packets, {len(self.drops)} drops")
    
    def get_dataframe(self):
        """Return packets as a pandas DataFrame."""
        return pd.DataFrame(self.packets)
    
    def detect_recovery_events(self, gap_threshold_s=10.0):
        """
        Detect node failure/recovery events based on gaps in packet reception.
        
        A "recovery event" is detected when:
        1. A node stops sending packets for > gap_threshold_s
        2. The node then resumes sending
        
        Args:
            gap_threshold_s: Minimum gap duration to consider as failure (default: 10s)
        
        Returns:
            List of recovery events: [{
                'node_id': str,
                'last_packet_time': str,
                'first_packet_time': str,
                'gap_duration_s': float,
                'packets_before': int (seq of last packet before gap),
                'packets_after': int (seq of first packet after gap)
            }]
        """
        df = self.get_dataframe()
        if df.empty or 'timestamp' not in df.columns:
            return []
        
        events = []
        
        for node_id in df['src_id'].unique():
            node_df = df[df['src_id'] == node_id].copy()
            
            # Convert timestamps to seconds
            node_df['time_s'] = node_df['timestamp'].apply(MetricsPlotter.timestamp_to_seconds)
            node_df = node_df.dropna(subset=['time_s']).sort_values('time_s')
            
            if len(node_df) < 2:
                continue
            
            # Find gaps
            times = node_df['time_s'].tolist()
            seqs = node_df['seq'].tolist()
            timestamps = node_df['timestamp'].tolist()
            
            for i in range(1, len(times)):
                gap = times[i] - times[i-1]
                if gap >= gap_threshold_s:
                    events.append({
                        'node_id': node_id,
                        'last_packet_time': timestamps[i-1],
                        'first_packet_time': timestamps[i],
                        'gap_duration_s': round(gap, 2),
                        'seq_before': seqs[i-1],
                        'seq_after': seqs[i],
                        'packets_lost_estimate': (seqs[i] - seqs[i-1] - 1) % 256
                    })
        
        return sorted(events, key=lambda x: x['gap_duration_s'], reverse=True)
    
    def get_stats(self):
        """Calculate summary statistics."""
        df = self.get_dataframe()
        if df.empty:
            return {}
        
        stats = {
            'total_packets': len(df),
            'total_drops': len(self.drops),
            'unique_nodes': df['src_id'].nunique(),
            'nodes': list(df['src_id'].unique())
        }
        
        # Latency stats
        latencies = df['latency_ms'].dropna()
        if not latencies.empty:
            stats['latency_min'] = latencies.min()
            stats['latency_max'] = latencies.max()
            stats['latency_mean'] = latencies.mean()
            stats['latency_median'] = latencies.median()
            stats['latency_std'] = latencies.std()
        
        # Hop stats
        hops = df['hops']
        if not hops.empty:
            stats['hops_min'] = hops.min()
            stats['hops_max'] = hops.max()
            stats['hops_mean'] = hops.mean()
            
            # Per-hop latency analysis (for multi-hop scenarios)
            latency_by_hop = {}
            for hop_count in hops.unique():
                hop_latencies = df[df['hops'] == hop_count]['latency_ms'].dropna()
                if not hop_latencies.empty:
                    latency_by_hop[int(hop_count)] = {
                        'mean': hop_latencies.mean(),
                        'median': hop_latencies.median(),
                        'std': hop_latencies.std(),
                        'count': len(hop_latencies)
                    }
            stats['latency_by_hop'] = latency_by_hop
        
        # PDR calculation (requires sequence analysis)
        # Process in chronological order to detect resets (seq drops)
        pdr_by_node = {}
        for src_id in df['src_id'].unique():
            node_df = df[df['src_id'] == src_id].sort_values('timestamp')
            if len(node_df) < 2:
                pdr_by_node[src_id] = 100.0
                continue
            
            seqs = node_df['seq'].tolist()
            total_received = len(seqs)
            total_missing = 0
            
            for i in range(1, len(seqs)):
                prev_seq, curr_seq = seqs[i-1], seqs[i]
                # Detect reset: seq drops (current < previous, not a wrap from 255->0)
                if curr_seq < prev_seq:
                    # Reset detected, start new session - no gap counted
                    continue
                gap = curr_seq - prev_seq
                if gap > 1:
                    total_missing += gap - 1
            
            total = total_received + total_missing
            pdr_by_node[src_id] = (total_received / total) * 100 if total > 0 else 100.0
        
        stats['pdr_by_node'] = pdr_by_node
        stats['pdr_overall'] = sum(pdr_by_node.values()) / len(pdr_by_node) if pdr_by_node else 100.0
        
        # Throughput calculation (bytes per second)
        # SENSOR_PAYLOAD_SIZE = 11 bytes, MESH_HEADER_SIZE = 10 bytes
        PACKET_SIZE_BYTES = 21  # 10-byte header + 11-byte payload
        
        # Calculate test duration from timestamps if available
        timestamps = df['timestamp'].dropna()
        if len(timestamps) >= 2:
            start_ts = MetricsPlotter.timestamp_to_seconds(timestamps.iloc[0])
            end_ts = MetricsPlotter.timestamp_to_seconds(timestamps.iloc[-1])
            if start_ts is not None and end_ts is not None and end_ts > start_ts:
                duration_s = end_ts - start_ts
                stats['duration_s'] = duration_s
                stats['throughput_packets_per_s'] = len(df) / duration_s
                stats['throughput_bytes_per_s'] = (len(df) * PACKET_SIZE_BYTES) / duration_s
                stats['throughput_bits_per_s'] = stats['throughput_bytes_per_s'] * 8
        
        # Recovery events (for Scenario 6)
        recovery_events = self.detect_recovery_events(gap_threshold_s=10.0)
        if recovery_events:
            stats['recovery_events'] = recovery_events
            stats['total_recovery_events'] = len(recovery_events)
            stats['max_recovery_time_s'] = max(e['gap_duration_s'] for e in recovery_events)
            stats['avg_recovery_time_s'] = sum(e['gap_duration_s'] for e in recovery_events) / len(recovery_events)
        
        return stats


class MetricsPlotter:
    """Generates graphs from parsed metrics."""
    
    def __init__(self, parser: MetricsParser, output_dir: Path):
        self.parser = parser
        self.output_dir = output_dir
        self.output_dir.mkdir(parents=True, exist_ok=True)
        
        # Set style
        plt.style.use('seaborn-v0_8-darkgrid')
        plt.rcParams['figure.figsize'] = (10, 6)
        plt.rcParams['font.size'] = 12
    
    @staticmethod
    def timestamp_to_seconds(ts_str):
        """Convert HH:MM:SS.mmm timestamp to seconds since midnight."""
        if not ts_str:
            return None
        try:
            parts = ts_str.split(':')
            hours = int(parts[0])
            minutes = int(parts[1])
            sec_parts = parts[2].split('.')
            seconds = int(sec_parts[0])
            millis = int(sec_parts[1]) if len(sec_parts) > 1 else 0
            return hours * 3600 + minutes * 60 + seconds + millis / 1000.0
        except:
            return None
    
    def plot_all(self):
        """Generate all graphs."""
        df = self.parser.get_dataframe()
        if df.empty:
            print("No data to plot!")
            return
        
        self.plot_latency_histogram(df)
        self.plot_latency_timeline(df)
        self.plot_pdr_by_node()
        self.plot_hop_distribution(df)
        self.plot_latency_by_hop(df)  # Per-hop latency analysis
        self.plot_hop_performance_comparison(df)  # NEW: Hop count performance table
        self.plot_throughput_timeline(df)
        self.plot_packet_loss_by_time(df)
        self.plot_packet_loss_by_index(df)
        self.export_csv(df)
        self.export_hop_comparison_csv(df)  # NEW: Per-hop CSV export
    
    def plot_latency_histogram(self, df):
        """Graph 1: Latency distribution histogram."""
        latencies = df['latency_ms'].dropna()
        if latencies.empty:
            print("No latency data for histogram")
            return
        
        fig, ax = plt.subplots()
        ax.hist(latencies, bins=30, color='#60a5fa', edgecolor='#1e3a5f', alpha=0.8)
        ax.axvline(latencies.mean(), color='#ef4444', linestyle='--', linewidth=2, label=f'Mean: {latencies.mean():.0f} ms')
        ax.axvline(latencies.median(), color='#22c55e', linestyle='--', linewidth=2, label=f'Median: {latencies.median():.0f} ms')
        
        ax.set_xlabel('Latency (ms)')
        ax.set_ylabel('Frequency')
        ax.set_title('LoRa Mesh Latency Distribution')
        ax.legend()
        
        fig.savefig(self.output_dir / 'latency_histogram.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'latency_histogram.png'}")
    
    def plot_latency_timeline(self, df):
        """Graph 2: Latency over time (by packet index)."""
        latencies = df['latency_ms'].dropna()
        if latencies.empty:
            return
        
        fig, ax = plt.subplots()
        ax.plot(range(len(latencies)), latencies, color='#60a5fa', linewidth=1, alpha=0.8)
        ax.axhline(latencies.mean(), color='#ef4444', linestyle='--', linewidth=1, label=f'Mean: {latencies.mean():.0f} ms')
        
        ax.set_xlabel('Packet Index')
        ax.set_ylabel('Latency (ms)')
        ax.set_title('LoRa Mesh Latency Over Time')
        ax.legend()
        
        fig.savefig(self.output_dir / 'latency_timeline.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'latency_timeline.png'}")
    
    def plot_pdr_by_node(self):
        """Graph 3: Per-node PDR bar chart."""
        stats = self.parser.get_stats()
        pdr_data = stats.get('pdr_by_node', {})
        
        if not pdr_data:
            return
        
        fig, ax = plt.subplots()
        nodes = list(pdr_data.keys())
        pdrs = list(pdr_data.values())
        colors = ['#22c55e' if p >= 95 else ('#eab308' if p >= 80 else '#ef4444') for p in pdrs]
        
        bars = ax.bar(nodes, pdrs, color=colors, edgecolor='#1e3a5f')
        
        ax.set_xlabel('Node ID')
        ax.set_ylabel('PDR (%)')
        ax.set_title('Packet Delivery Ratio by Node')
        ax.set_ylim(0, 105)
        
        # Add value labels
        for bar, pdr in zip(bars, pdrs):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1, 
                   f'{pdr:.1f}%', ha='center', va='bottom', fontsize=10)
        
        fig.savefig(self.output_dir / 'pdr_by_node.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'pdr_by_node.png'}")
    
    def plot_hop_distribution(self, df):
        """Graph 4: Hop count histogram."""
        hops = df['hops']
        if hops.empty:
            return
        
        fig, ax = plt.subplots()
        hop_counts = hops.value_counts().sort_index()
        ax.bar(hop_counts.index, hop_counts.values, color='#a855f7', edgecolor='#581c87')
        
        ax.set_xlabel('Hop Count')
        ax.set_ylabel('Frequency')
        ax.set_title('Hop Count Distribution')
        ax.set_xticks(range(int(hops.min()), int(hops.max()) + 1))
        
        fig.savefig(self.output_dir / 'hop_distribution.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'hop_distribution.png'}")
    
    def plot_latency_by_hop(self, df):
        """Graph 4b: Latency distribution grouped by hop count (for multi-hop analysis)."""
        df = df.dropna(subset=['latency_ms'])
        if df.empty or df['hops'].nunique() < 2:
            print("Insufficient hop diversity for per-hop latency graph")
            return
        
        # Group by hop count
        hop_groups = df.groupby('hops')['latency_ms']
        
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
        
        # Box plot (left)
        hop_data = [hop_groups.get_group(h).values for h in sorted(df['hops'].unique())]
        hop_labels = [f'{int(h)} hop{"s" if h > 1 else ""}' for h in sorted(df['hops'].unique())]
        
        bp = ax1.boxplot(hop_data, labels=hop_labels, patch_artist=True)
        colors = ['#60a5fa', '#22c55e', '#eab308', '#ef4444', '#a855f7']
        for patch, color in zip(bp['boxes'], colors[:len(bp['boxes'])]):
            patch.set_facecolor(color)
            patch.set_alpha(0.7)
        
        ax1.set_xlabel('Hop Count')
        ax1.set_ylabel('Latency (ms)')
        ax1.set_title('Latency Distribution by Hop Count')
        
        # Mean latency bar chart (right)
        mean_latencies = [hop_groups.get_group(h).mean() for h in sorted(df['hops'].unique())]
        hop_counts = sorted(df['hops'].unique())
        
        bars = ax2.bar(range(len(hop_counts)), mean_latencies, color=colors[:len(hop_counts)], alpha=0.7)
        ax2.set_xticks(range(len(hop_counts)))
        ax2.set_xticklabels(hop_labels)
        ax2.set_xlabel('Hop Count')
        ax2.set_ylabel('Mean Latency (ms)')
        ax2.set_title('Average Latency by Hop Count')
        
        # Add value labels and per-hop delay calculation
        for i, (bar, mean_lat) in enumerate(zip(bars, mean_latencies)):
            ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 5, 
                    f'{mean_lat:.0f} ms', ha='center', va='bottom', fontsize=10)
        
        # Calculate and display per-hop forwarding delay
        if len(mean_latencies) >= 2:
            per_hop_delays = [mean_latencies[i+1] - mean_latencies[i] for i in range(len(mean_latencies)-1)]
            avg_per_hop = sum(per_hop_delays) / len(per_hop_delays)
            ax2.text(0.95, 0.95, f'Avg per-hop delay: ~{avg_per_hop:.0f} ms', 
                    transform=ax2.transAxes, ha='right', va='top', fontsize=11,
                    bbox=dict(boxstyle='round', facecolor='white', alpha=0.8))
        
        fig.tight_layout()
        fig.savefig(self.output_dir / 'latency_by_hop.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'latency_by_hop.png'}")
    
    def plot_hop_performance_comparison(self, df):
        """Graph: Performance comparison table by hop count (similar to reference paper Table I)."""
        if 'hops' not in df.columns or df['hops'].dropna().empty:
            return
        
        df = df.copy()
        df['time_s'] = df['timestamp'].apply(self.timestamp_to_seconds)
        df = df.dropna(subset=['time_s', 'hops'])
        
        if df.empty:
            return
        
        hop_counts = sorted(df['hops'].unique())
        if len(hop_counts) < 1:
            return
        
        # Calculate metrics per hop count
        metrics_data = []
        bytes_per_packet = 21  # 10 header + 11 payload
        
        for hop in hop_counts:
            hop_df = df[df['hops'] == hop]
            n_packets = len(hop_df)
            
            # Calculate time range for throughput
            if n_packets > 1:
                duration = hop_df['time_s'].max() - hop_df['time_s'].min()
                if duration > 0:
                    throughput_pps = n_packets / duration
                    bandwidth_bps = (n_packets * bytes_per_packet) / duration
                else:
                    throughput_pps = 0
                    bandwidth_bps = 0
            else:
                throughput_pps = 0
                bandwidth_bps = 0
            
            # Latency stats
            latencies = hop_df['latency_ms'].dropna()
            latency_mean = latencies.mean() if not latencies.empty else 0
            latency_std = latencies.std() if len(latencies) > 1 else 0
            
            # RSSI stats
            rssi_mean = hop_df['rssi'].mean() if 'rssi' in hop_df.columns else 0
            
            metrics_data.append({
                'Hops': hop,
                'Packets': n_packets,
                'Throughput (pkt/s)': throughput_pps,
                'Bandwidth (B/s)': bandwidth_bps,
                'Latency Mean (ms)': latency_mean,
                'Latency Std (ms)': latency_std,
                'RSSI Mean (dBm)': rssi_mean
            })
        
        # Create table figure
        fig, ax = plt.subplots(figsize=(12, 3 + len(hop_counts) * 0.4))
        ax.axis('off')
        
        # Create table
        columns = ['Hops', 'Packets', 'Throughput\n(pkt/s)', 'Bandwidth\n(B/s)', 
                   'Latency Mean\n(ms)', 'Latency Std\n(ms)', 'RSSI Mean\n(dBm)']
        cell_text = []
        for m in metrics_data:
            cell_text.append([
                f"{m['Hops']}",
                f"{m['Packets']}",
                f"{m['Throughput (pkt/s)']:.3f}",
                f"{m['Bandwidth (B/s)']:.2f}",
                f"{m['Latency Mean (ms)']:.1f}",
                f"{m['Latency Std (ms)']:.1f}",
                f"{m['RSSI Mean (dBm)']:.1f}"
            ])
        
        table = ax.table(cellText=cell_text, colLabels=columns, loc='center', cellLoc='center')
        table.auto_set_font_size(False)
        table.set_fontsize(10)
        table.scale(1.2, 1.8)
        
        # Style header
        for i in range(len(columns)):
            table[(0, i)].set_facecolor('#3b82f6')
            table[(0, i)].set_text_props(color='white', fontweight='bold')
        
        # Alternate row colors
        for i in range(1, len(metrics_data) + 1):
            for j in range(len(columns)):
                if i % 2 == 0:
                    table[(i, j)].set_facecolor('#f0f9ff')
        
        ax.set_title('Network Performance Metrics by Hop Count', fontsize=14, fontweight='bold', pad=20)
        
        fig.tight_layout()
        fig.savefig(self.output_dir / 'hop_performance_table.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'hop_performance_table.png'}")
    
    def export_hop_comparison_csv(self, df):
        """Export per-hop performance metrics to CSV (similar to reference paper Table I)."""
        if 'hops' not in df.columns or df['hops'].dropna().empty:
            return
        
        df = df.copy()
        df['time_s'] = df['timestamp'].apply(self.timestamp_to_seconds)
        df = df.dropna(subset=['time_s', 'hops'])
        
        if df.empty:
            return
        
        hop_counts = sorted(df['hops'].unique())
        bytes_per_packet = 21
        
        rows = []
        for hop in hop_counts:
            hop_df = df[df['hops'] == hop]
            n_packets = len(hop_df)
            
            if n_packets > 1:
                duration = hop_df['time_s'].max() - hop_df['time_s'].min()
                throughput_pps = n_packets / duration if duration > 0 else 0
                bandwidth_bps = (n_packets * bytes_per_packet) / duration if duration > 0 else 0
            else:
                throughput_pps = 0
                bandwidth_bps = 0
            
            latencies = hop_df['latency_ms'].dropna()
            
            rows.append({
                'Hop Count': hop,
                'Packet Count': n_packets,
                'Throughput (packets/s)': round(throughput_pps, 4),
                'Bandwidth (bytes/s)': round(bandwidth_bps, 2),
                'Latency Mean (ms)': round(latencies.mean(), 1) if not latencies.empty else None,
                'Latency Median (ms)': round(latencies.median(), 1) if not latencies.empty else None,
                'Latency Std (ms)': round(latencies.std(), 1) if len(latencies) > 1 else None,
                'Latency Min (ms)': round(latencies.min(), 0) if not latencies.empty else None,
                'Latency Max (ms)': round(latencies.max(), 0) if not latencies.empty else None,
                'RSSI Mean (dBm)': round(hop_df['rssi'].mean(), 1) if 'rssi' in hop_df.columns else None
            })
        
        hop_df = pd.DataFrame(rows)
        hop_df.to_csv(self.output_dir / 'performance_by_hop.csv', index=False)
        print(f"Saved: {self.output_dir / 'performance_by_hop.csv'}")

    def plot_throughput_timeline(self, df):
        """Graph 5: Throughput over time using actual timestamps."""
        if len(df) < 5:
            return
        
        # Convert timestamps to seconds
        df = df.copy()
        df['time_s'] = df['timestamp'].apply(self.timestamp_to_seconds)
        df = df.dropna(subset=['time_s'])
        
        if len(df) < 5:
            print("Not enough timestamped packets for throughput graph")
            return
        
        # Normalize to start at 0
        start_time = df['time_s'].min()
        df['elapsed_s'] = df['time_s'] - start_time
        
        # Packet size: 10-byte mesh header + 11-byte sensor payload = 21 bytes
        bytes_per_packet = 21  # MESH_HEADER_SIZE + SENSOR_PAYLOAD_SIZE
        
        # Rolling window in seconds
        window_s = 30.0
        
        elapsed_times = df['elapsed_s'].tolist()
        throughput = []
        time_axis = []
        
        for i, t in enumerate(elapsed_times):
            # Skip until window has warmed up (at least 10s of data)
            if t < 10.0:
                continue
            
            # Count packets in the window [t - window_s, t]
            window_start = max(0, t - window_s)
            packets_in_window = sum(1 for et in elapsed_times[:i+1] if et >= window_start)
            
            # Calculate actual window duration
            times_in_window = [et for et in elapsed_times[:i+1] if et >= window_start]
            if len(times_in_window) >= 2:
                actual_window = times_in_window[-1] - times_in_window[0]
                # Require minimum 5s window to avoid spikes
                if actual_window >= 5.0:
                    bps = (packets_in_window * bytes_per_packet) / actual_window
                else:
                    continue
            else:
                continue
            
            throughput.append(bps)
            time_axis.append(t)
        
        fig, ax = plt.subplots()
        ax.plot(time_axis, throughput, color='#22c55e', linewidth=2)
        ax.fill_between(time_axis, throughput, alpha=0.3, color='#22c55e')
        
        ax.set_xlabel('Elapsed Time (seconds)')
        ax.set_ylabel('Throughput (B/s)')
        ax.set_title('LoRa Mesh Throughput Over Time (30s Rolling Window)')
        
        # Add average line
        avg_throughput = sum(throughput) / len(throughput) if throughput else 0
        ax.axhline(avg_throughput, color='#ef4444', linestyle='--', linewidth=1.5, 
                   label=f'Avg: {avg_throughput:.1f} B/s ({avg_throughput * 8:.0f} bps)')
        ax.legend()
        
        fig.savefig(self.output_dir / 'throughput_timeline.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'throughput_timeline.png'}")
    
    def plot_packet_loss_by_time(self, df):
        """Graph 6a: Cumulative packet loss over elapsed time."""
        df = df.copy()
        df['time_s'] = df['timestamp'].apply(self.timestamp_to_seconds)
        df = df.dropna(subset=['time_s'])
        
        if len(df) < 2:
            return
        
        # Sort by timestamp (chronological order across all nodes)
        df = df.sort_values('time_s').reset_index(drop=True)
        start_time = df['time_s'].min()
        df['elapsed_s'] = df['time_s'] - start_time
        
        # Track per-node sequence numbers
        last_seq = {}
        cumulative_loss = []
        elapsed_times = []
        loss_total = 0
        
        for _, row in df.iterrows():
            src_id = row['src_id']
            seq = row['seq']
            
            if src_id in last_seq:
                gap = (seq - last_seq[src_id]) % 256
                if gap > 1 and gap < 50:  # Reasonable gap
                    loss_total += gap - 1
            
            last_seq[src_id] = seq
            cumulative_loss.append(loss_total)
            elapsed_times.append(row['elapsed_s'])
        
        fig, ax = plt.subplots()
        ax.step(elapsed_times, cumulative_loss, where='post', color='#ef4444', linewidth=2)
        ax.fill_between(elapsed_times, cumulative_loss, step='post', alpha=0.3, color='#ef4444')
        
        ax.set_xlabel('Elapsed Time (seconds)')
        ax.set_ylabel('Cumulative Packets Lost')
        ax.set_title('Cumulative Packet Loss Over Time')
        
        # Add final count annotation
        if cumulative_loss:
            ax.annotate(f'Total: {cumulative_loss[-1]}', 
                       xy=(elapsed_times[-1], cumulative_loss[-1]),
                       xytext=(5, 5), textcoords='offset points',
                       fontsize=10, color='#ef4444')
        
        fig.savefig(self.output_dir / 'packet_loss_by_time.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'packet_loss_by_time.png'}")
    
    def plot_packet_loss_by_index(self, df):
        """Graph 6b: Cumulative packet loss by packet reception order."""
        df = df.copy()
        
        # Sort by line_num (reception order)
        df = df.sort_values('line_num').reset_index(drop=True)
        
        # Track per-node sequence numbers
        last_seq = {}
        cumulative_loss = []
        loss_total = 0
        
        for _, row in df.iterrows():
            src_id = row['src_id']
            seq = row['seq']
            
            if src_id in last_seq:
                gap = (seq - last_seq[src_id]) % 256
                if gap > 1 and gap < 50:  # Reasonable gap
                    loss_total += gap - 1
            
            last_seq[src_id] = seq
            cumulative_loss.append(loss_total)
        
        if not cumulative_loss:
            return
        
        fig, ax = plt.subplots()
        ax.step(range(len(cumulative_loss)), cumulative_loss, where='post', color='#ef4444', linewidth=2)
        ax.fill_between(range(len(cumulative_loss)), cumulative_loss, step='post', alpha=0.3, color='#ef4444')
        
        ax.set_xlabel('Packet Index (Reception Order)')
        ax.set_ylabel('Cumulative Packets Lost')
        ax.set_title('Cumulative Packet Loss by Reception Order')
        
        # Add final count annotation
        if cumulative_loss:
            ax.annotate(f'Total: {cumulative_loss[-1]}', 
                       xy=(len(cumulative_loss)-1, cumulative_loss[-1]),
                       xytext=(5, 5), textcoords='offset points',
                       fontsize=10, color='#ef4444')
        
        fig.savefig(self.output_dir / 'packet_loss_cumulative.png', dpi=150, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved: {self.output_dir / 'packet_loss_cumulative.png'}")
    
    def export_csv(self, df):
        """Export summary statistics to CSV."""
        stats = self.parser.get_stats()
        
        # Create summary DataFrame
        summary = {
            'Metric': ['Total Packets', 'Total Drops', 'Unique Nodes', 'Overall PDR (%)'],
            'Value': [
                stats.get('total_packets', 0),
                stats.get('total_drops', 0),
                stats.get('unique_nodes', 0),
                f"{stats.get('pdr_overall', 100):.1f}"
            ]
        }
        
        # Add latency stats
        if 'latency_mean' in stats:
            summary['Metric'].extend(['Latency Min (ms)', 'Latency Max (ms)', 'Latency Mean (ms)', 'Latency Median (ms)', 'Latency Std Dev'])
            summary['Value'].extend([
                f"{stats['latency_min']:.0f}",
                f"{stats['latency_max']:.0f}",
                f"{stats['latency_mean']:.1f}",
                f"{stats['latency_median']:.1f}",
                f"{stats['latency_std']:.1f}"
            ])
        
        # Add hop stats
        if 'hops_mean' in stats:
            summary['Metric'].extend(['Hops Min', 'Hops Max', 'Hops Mean'])
            summary['Value'].extend([
                stats['hops_min'],
                stats['hops_max'],
                f"{stats['hops_mean']:.1f}"
            ])
        
        # Add throughput stats
        if 'throughput_bytes_per_s' in stats:
            summary['Metric'].extend(['Test Duration (s)', 'Throughput (packets/s)', 'Throughput (bytes/s)', 'Throughput (bps)'])
            summary['Value'].extend([
                f"{stats['duration_s']:.1f}",
                f"{stats['throughput_packets_per_s']:.2f}",
                f"{stats['throughput_bytes_per_s']:.1f}",
                f"{stats['throughput_bits_per_s']:.0f}"
            ])
        
        # Add per-hop latency analysis
        if 'latency_by_hop' in stats and stats['latency_by_hop']:
            for hop_count, hop_stats in sorted(stats['latency_by_hop'].items()):
                summary['Metric'].append(f'Latency @ {hop_count} hop(s) - Mean (ms)')
                summary['Value'].append(f"{hop_stats['mean']:.1f}")
                summary['Metric'].append(f'Latency @ {hop_count} hop(s) - Count')
                summary['Value'].append(str(hop_stats['count']))
        
        # Add recovery event stats (Scenario 6)
        if 'total_recovery_events' in stats:
            summary['Metric'].extend(['Recovery Events Detected', 'Max Recovery Time (s)', 'Avg Recovery Time (s)'])
            summary['Value'].extend([
                str(stats['total_recovery_events']),
                f"{stats['max_recovery_time_s']:.1f}",
                f"{stats['avg_recovery_time_s']:.1f}"
            ])
        
        summary_df = pd.DataFrame(summary)
        summary_df.to_csv(self.output_dir / 'summary.csv', index=False)
        print(f"Saved: {self.output_dir / 'summary.csv'}")
        
        # Also save raw packet data
        df.to_csv(self.output_dir / 'packets.csv', index=False)
        print(f"Saved: {self.output_dir / 'packets.csv'}")
        
        # Save recovery events to separate CSV if detected (Scenario 6)
        if 'recovery_events' in stats and stats['recovery_events']:
            recovery_df = pd.DataFrame(stats['recovery_events'])
            recovery_df.to_csv(self.output_dir / 'recovery_events.csv', index=False)
            print(f"Saved: {self.output_dir / 'recovery_events.csv'}")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze LoRa mesh serial logs and generate metrics graphs.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python metrics_analysis.py logs/test_run.log --output report/
    python metrics_analysis.py logs/*.log --compare --output comparison/
        """
    )
    parser.add_argument('files', nargs='+', help='Log file(s) to analyze')
    parser.add_argument('--output', '-o', default='report/', help='Output directory (default: report/)')
    parser.add_argument('--compare', action='store_true', help='Compare multiple log files')
    
    args = parser.parse_args()
    
    output_dir = Path(args.output)
    
    if args.compare and len(args.files) > 1:
        # Multi-file comparison mode
        print(f"Comparing {len(args.files)} log files...")
        # TODO: Implement comparison mode
        print("Comparison mode not yet implemented. Analyzing first file only.")
        args.files = [args.files[0]]
    
    # Parse all files
    metrics_parser = MetricsParser()
    for filepath in args.files:
        path = Path(filepath)
        if path.exists():
            metrics_parser.parse_file(path)
        else:
            print(f"Warning: File not found: {filepath}")
    
    if not metrics_parser.packets:
        print("No packets found in log files!")
        sys.exit(1)
    
    # Print summary stats
    stats = metrics_parser.get_stats()
    print("\n" + "=" * 50)
    print("SUMMARY STATISTICS")
    print("=" * 50)
    print(f"Total packets: {stats.get('total_packets', 0)}")
    print(f"Total drops: {stats.get('total_drops', 0)}")
    print(f"Unique nodes: {stats.get('unique_nodes', 0)} ({', '.join(stats.get('nodes', []))})")
    print(f"Overall PDR: {stats.get('pdr_overall', 100):.1f}%")
    
    if 'latency_mean' in stats:
        print(f"\nLatency:")
        print(f"  Min: {stats['latency_min']:.0f} ms")
        print(f"  Max: {stats['latency_max']:.0f} ms")
        print(f"  Mean: {stats['latency_mean']:.1f} ms")
        print(f"  Median: {stats['latency_median']:.1f} ms")
    
    print("=" * 50 + "\n")
    
    # Print formatted metrics table (similar to reference paper Table I)
    print_metrics_table(stats)
    
    # Generate plots
    plotter = MetricsPlotter(metrics_parser, output_dir)
    plotter.plot_all()
    
    print(f"\nAll outputs saved to: {output_dir.absolute()}")


def print_metrics_table(stats):
    """Print a formatted metrics table similar to reference paper Table I."""
    print("\n" + "=" * 60)
    print("NETWORK PERFORMANCE METRICS TABLE")
    print("=" * 60)
    print(f"{'Metric':<35} {'Value':>12} {'Unit':>10}")
    print("-" * 60)
    
    # Core metrics
    print(f"{'Total Packets Received':<35} {stats.get('total_packets', 0):>12} {'packets':>10}")
    print(f"{'Total Packets Dropped':<35} {stats.get('total_drops', 0):>12} {'packets':>10}")
    print(f"{'Packet Delivery Ratio (PDR)':<35} {stats.get('pdr_overall', 100):>11.1f}% {'':<10}")
    
    # Throughput metrics
    if 'throughput_packets_per_s' in stats:
        print("-" * 60)
        print(f"{'Test Duration':<35} {stats['duration_s']:>11.1f}s {'':<10}")
        print(f"{'Throughput':<35} {stats['throughput_packets_per_s']:>11.3f} {'packets/s':>10}")
        print(f"{'Bandwidth':<35} {stats['throughput_bytes_per_s']:>11.2f} {'bytes/s':>10}")
        print(f"{'Bandwidth (bps)':<35} {stats['throughput_bits_per_s']:>11.0f} {'bps':>10}")
    
    # Latency metrics
    if 'latency_mean' in stats:
        print("-" * 60)
        print(f"{'Latency (Min)':<35} {stats['latency_min']:>11.0f} {'ms':>10}")
        print(f"{'Latency (Max)':<35} {stats['latency_max']:>11.0f} {'ms':>10}")
        print(f"{'Latency (Mean)':<35} {stats['latency_mean']:>11.1f} {'ms':>10}")
        print(f"{'Latency (Median)':<35} {stats['latency_median']:>11.1f} {'ms':>10}")
        print(f"{'Latency (Std Dev)':<35} {stats['latency_std']:>11.1f} {'ms':>10}")
    
    # Hop metrics
    if 'hops_mean' in stats:
        print("-" * 60)
        print(f"{'Hop Count (Min)':<35} {stats['hops_min']:>12} {'hops':>10}")
        print(f"{'Hop Count (Max)':<35} {stats['hops_max']:>12} {'hops':>10}")
        print(f"{'Hop Count (Mean)':<35} {stats['hops_mean']:>11.1f} {'hops':>10}")
    
    # Per-hop latency analysis
    if 'latency_by_hop' in stats and stats['latency_by_hop']:
        print("-" * 60)
        print("Per-Hop Latency Analysis:")
        for hop_count, hop_stats in sorted(stats['latency_by_hop'].items()):
            print(f"  {hop_count} hop(s): mean={hop_stats['mean']:.1f}ms, "
                  f"std={hop_stats['std']:.1f}ms, n={hop_stats['count']}")
    
    # Recovery events (Scenario 6)
    if 'total_recovery_events' in stats and stats['total_recovery_events'] > 0:
        print("-" * 60)
        print(f"{'Recovery Events Detected':<35} {stats['total_recovery_events']:>12} {'events':>10}")
        print(f"{'Max Recovery Time':<35} {stats['max_recovery_time_s']:>11.1f} {'seconds':>10}")
        print(f"{'Avg Recovery Time':<35} {stats['avg_recovery_time_s']:>11.1f} {'seconds':>10}")
        print("-" * 60)
        print("Recovery Event Details:")
        for i, event in enumerate(stats['recovery_events'][:5], 1):  # Show top 5
            print(f"  {i}. Node 0x{event['node_id']}: {event['gap_duration_s']:.1f}s gap "
                  f"({event['last_packet_time']} → {event['first_packet_time']})")
        if len(stats['recovery_events']) > 5:
            print(f"  ... and {len(stats['recovery_events']) - 5} more (see recovery_events.csv)")
    
    print("=" * 60 + "\n")


if __name__ == '__main__':
    main()
