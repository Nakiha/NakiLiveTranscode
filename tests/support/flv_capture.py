"""Bounded, streaming FLV structural verification, independent of demux/remux."""
import errno
import json


def audio_media(body):
    if not body:
        raise ValueError('Short FLV audio tag')
    codec = body[0] >> 4
    if codec in (2, 14):  # legacy MP3, including 8 kHz
        return 1
    if codec == 10:
        if len(body) < 2:
            raise ValueError('Short AAC tag')
        return int(body[1] == 1)
    if codec != 9:
        raise ValueError('Unsupported FLV audio codec')
    kind = body[0] & 15
    offset = 1
    if kind == 5:  # AudioPacketTypeMultitrack, mux emits OneTrack per tag
        if len(body) < 7 or body[1] & 0xf0:
            raise ValueError('Invalid/unsupported enhanced audio track header')
        kind = body[1] & 15
        offset = 2
    if len(body) < offset + 4 or body[offset:offset + 4] not in (b'mp4a', b'.mp3'):
        raise ValueError('Invalid/unsupported enhanced audio codec')
    return int(kind == 1)  # configuration/channel metadata are not media


def inspect_flv(path):
    counts = dict(video=0, audio=0, tags=0)
    with path.open('rb') as stream:
        header = stream.read(9)
        if len(header) != 9 or header[:4] != b'FLV\x01' or header[4] & ~5:
            raise ValueError('Invalid FLV header')
        offset = int.from_bytes(header[5:9], 'big')
        if not 9 <= offset <= 1048576 or len(stream.read(offset - 9)) != offset - 9:
            raise ValueError('Invalid FLV header offset')
        if stream.read(4) != bytes(4):
            raise ValueError('Invalid first PreviousTagSize')
        while True:
            tag = stream.read(11)
            if not tag:
                break
            if len(tag) != 11 or tag[0] not in (8, 9, 18) or tag[8:11] != bytes(3):
                raise ValueError('Invalid/truncated FLV tag header')
            size = int.from_bytes(tag[1:4], 'big')
            if size > 4 * 1024 * 1024:
                raise ValueError('FLV tag exceeds verification bound')
            body = stream.read(size)
            trailer = stream.read(4)
            if len(body) != size or len(trailer) != 4 or int.from_bytes(trailer, 'big') != 11 + size:
                raise ValueError('Truncated FLV tag or invalid PreviousTagSize')
            counts['tags'] += 1
            if tag[0] == 9:
                if len(body) < 2:
                    raise ValueError('Short FLV video tag')
                if body[0] & 128:
                    if len(body) < 5:
                        raise ValueError('Short enhanced video tag')
                    media = body[0] & 15 in (1, 3)
                else:
                    media = body[1] == 1
                counts['video'] += int(media)
            elif tag[0] == 8:
                counts['audio'] += audio_media(body)
    if not counts['video']:
        raise ValueError('No coded video in FLV capture')
    return counts


def receipt(log, returncode):
    rows = [json.loads(line) for line in log.splitlines() if line.startswith('{')]
    rows = [row for row in rows if row.get('event') == 'capture_complete']
    if len(rows) != 1:
        raise ValueError('Missing/duplicate capture completion receipt')
    row = rows[0]
    expected = {0: ('complete', 'eof'), 12: ('unconfirmed', 'rtmp_peer_close_unconfirmed')}
    allowed_close = (0, -errno.EPIPE, -errno.EIO) if returncode == 12 and row['read_result'] == -errno.EIO else (0,)
    if returncode not in expected or (row['status'], row['reason']) != expected[returncode] or row['close_result'] not in allowed_close:
        raise ValueError('Capture transport failed: ' + str(row))
    return dict(row, returncode=returncode)


def verify_capture(path, transport, video_packets, audio_packets, controlled_stop):
    if not controlled_stop or transport['bytes'] != path.stat().st_size:
        raise ValueError('Capture lacks controlled stop or byte accounting')
    counts = inspect_flv(path)
    if (counts['video'], counts['audio']) != (video_packets, audio_packets):
        raise ValueError('FLV/service packet count mismatch: ' + str(counts))
    return dict(status='verified_controlled_close', transport=transport, flv=counts)
