"""The little of MPEG-TS the client needs to publish a fixture: where its random
access points are, and when each packet of a constant-rate stream is due."""

PACKET = 188
# The video PIDs of the runner's fixtures (lib/media/mpeg-ts/make-fixture.sh).
VIDEO_PIDS = (0x100, 0x211, 0x311)


def packets(data):
    return [data[i:i + PACKET] for i in range(0, len(data) - PACKET + 1, PACKET)]


def pid_of(packet):
    return ((packet[1] & 0x1F) << 8) | packet[2]


def starts_random_access(packet):
    """A video PES start with random_access_indicator."""
    return (pid_of(packet) in VIDEO_PIDS and bool(packet[1] & 0x40) and bool(packet[3] & 0x20)
            and packet[4] > 0 and bool(packet[5] & 0x40))


def first_random_access(packet_list, start=0):
    for index in range(start, len(packet_list)):
        if starts_random_access(packet_list[index]):
            return index
    raise ValueError(f'no random access point at or after packet {start}')


def paced_chunks(data, duration_s, start, chunk_packets=40):
    """(seconds after T0, bytes) from packet `start` on, at the constant rate
    that plays the whole of `data` in `duration_s`."""
    total = len(data) // PACKET
    for index in range(start, total, chunk_packets):
        yield index / total * duration_s, data[index * PACKET:(index + chunk_packets) * PACKET]
