#pragma once

#include "openxmb/media/clock.hpp"
#include "openxmb/media/error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxmb::media {

using MediaId = std::string;

struct ImageReference {
  std::string uri;
  std::uint32_t pixel_width{};
  std::uint32_t pixel_height{};
  std::string accessible_description;
};

enum class CodecKind { container, video, audio, subtitle };

struct CodecBadge {
  CodecKind kind{CodecKind::container};
  std::string label;
  std::string semantic_icon;
};

struct TechnicalMetadata {
  std::string container;
  std::string video_codec;
  std::string audio_codec;
  std::uint64_t bit_rate{};
  std::uint32_t pixel_width{};
  std::uint32_t pixel_height{};
  std::uint32_t sample_rate{};
  std::uint32_t channel_count{};
  std::vector<CodecBadge> badges;
};

enum class PhotoGroupKind {
  album,
  year,
  month,
  day,
  camera,
  folder,
  playlist,
};

struct Photo {
  MediaId id;
  std::string title;
  std::string source_uri;
  std::optional<ImageReference> thumbnail;
  std::optional<std::chrono::sys_seconds> captured_at;
  std::string camera_model;
  std::vector<std::string> tags;
  std::vector<MediaId> group_ids;
  std::uint16_t orientation_degrees{};
  std::uint32_t pixel_width{};
  std::uint32_t pixel_height{};
};

struct PhotoGroup {
  MediaId id;
  std::string title;
  PhotoGroupKind kind{PhotoGroupKind::album};
  std::vector<MediaId> photo_ids;
  std::optional<MediaId> cover_photo_id;
};

struct MusicArtist {
  MediaId id;
  std::string name;
  std::vector<MediaId> album_ids;
  std::optional<ImageReference> portrait;
};

struct MusicAlbum {
  MediaId id;
  std::string title;
  std::vector<MediaId> artist_ids;
  std::vector<MediaId> track_ids;
  std::optional<ImageReference> artwork;
  std::optional<std::int32_t> release_year;
  std::string genre;
};

struct MusicTrack {
  MediaId id;
  std::string title;
  std::string source_uri;
  std::vector<MediaId> artist_ids;
  std::optional<MediaId> album_id;
  std::optional<ImageReference> artwork;
  std::optional<MediaTime> duration;
  std::uint16_t disc_number{1};
  std::uint16_t track_number{};
  std::string genre;
  TechnicalMetadata technical;
};

struct MusicPlaylist {
  MediaId id;
  std::string title;
  std::vector<MediaId> track_ids;
  std::optional<ImageReference> artwork;
  bool user_editable{true};
};

struct VideoChapter {
  MediaId id;
  std::string title;
  MediaTime start{};
  std::optional<ImageReference> thumbnail;
};

struct SubtitleTrack {
  MediaId id;
  std::string label;
  std::string language;
  // Empty for an embedded stream. The backend-specific stream identifier is an
  // opaque string so FFmpeg types never cross this API boundary.
  std::optional<std::string> external_uri;
  std::string backend_stream_key;
  bool is_default{false};
  bool forced{false};
  bool closed_captions{false};
};

enum class AudioChannelMode { stereo, left_plus_right, left, right };

struct AlternateAudioTrack {
  MediaId id;
  std::string label;
  std::string language;
  std::optional<std::string> external_uri;
  std::string backend_stream_key;
  std::string codec;
  std::uint32_t channel_count{};
  bool is_default{false};
  bool audio_description{false};
};

struct ResumePoint {
  MediaTime position{};
  std::chrono::sys_seconds updated_at{};
};

struct Video {
  MediaId id;
  std::string title;
  std::string source_uri;
  std::optional<ImageReference> poster;
  std::optional<MediaTime> duration;
  std::vector<VideoChapter> chapters;
  std::vector<SubtitleTrack> subtitle_tracks;
  std::vector<AlternateAudioTrack> audio_tracks;
  std::optional<ResumePoint> resume;
  TechnicalMetadata technical;
  std::string folder_id;
  std::optional<std::chrono::sys_seconds> recorded_at;
};

struct CatalogInput {
  std::vector<Photo> photos;
  std::vector<PhotoGroup> photo_groups;
  std::vector<MusicArtist> artists;
  std::vector<MusicAlbum> albums;
  std::vector<MusicTrack> tracks;
  std::vector<MusicPlaylist> playlists;
  std::vector<Video> videos;
};

[[nodiscard]] MediaResult<void> validate_catalog(const CatalogInput &input);

class Catalog {
public:
  [[nodiscard]] static MediaResult<Catalog> create(CatalogInput input);

  [[nodiscard]] std::span<const Photo> photos() const noexcept;
  [[nodiscard]] std::span<const PhotoGroup> photo_groups() const noexcept;
  [[nodiscard]] std::span<const MusicArtist> artists() const noexcept;
  [[nodiscard]] std::span<const MusicAlbum> albums() const noexcept;
  [[nodiscard]] std::span<const MusicTrack> tracks() const noexcept;
  [[nodiscard]] std::span<const MusicPlaylist> playlists() const noexcept;
  [[nodiscard]] std::span<const Video> videos() const noexcept;

  [[nodiscard]] const Photo *find_photo(std::string_view id) const noexcept;
  [[nodiscard]] const MusicTrack *
  find_track(std::string_view id) const noexcept;
  [[nodiscard]] const Video *find_video(std::string_view id) const noexcept;

private:
  explicit Catalog(CatalogInput input) : input_(std::move(input)) {}
  CatalogInput input_;
};

} // namespace openxmb::media
