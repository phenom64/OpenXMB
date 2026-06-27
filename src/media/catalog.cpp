#include "openxmb/media/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace openxmb::media {
namespace {

using IdSet = std::unordered_set<std::string_view>;

[[nodiscard]] bool has_text(std::string_view text) noexcept {
  return std::ranges::any_of(
      text, [](unsigned char character) { return !std::isspace(character); });
}

[[nodiscard]] MediaResult<void> malformed(std::string detail,
                                          std::string subject = {}) {
  return MediaResult<void>::failure(
      make_media_error(MediaErrorCode::malformed_catalog, "validate_catalog",
                       std::move(detail), std::move(subject)));
}

[[nodiscard]] MediaResult<void> missing(std::string detail,
                                        std::string subject = {}) {
  return MediaResult<void>::failure(
      make_media_error(MediaErrorCode::missing_reference, "validate_catalog",
                       std::move(detail), std::move(subject)));
}

[[nodiscard]] MediaResult<void> duplicate(std::string detail,
                                          std::string subject = {}) {
  return MediaResult<void>::failure(
      make_media_error(MediaErrorCode::duplicate_id, "validate_catalog",
                       std::move(detail), std::move(subject)));
}

template <typename Range, typename IdAccessor>
[[nodiscard]] MediaResult<IdSet> collect_ids(const Range &range,
                                             std::string_view collection,
                                             IdAccessor id_accessor) {
  IdSet ids;
  ids.reserve(range.size());
  for (const auto &item : range) {
    const std::string_view id = id_accessor(item);
    if (!has_text(id)) {
      return MediaResult<IdSet>::failure(make_media_error(
          MediaErrorCode::malformed_catalog, "validate_catalog",
          std::string(collection) + " contains an empty identifier"));
    }
    if (!ids.insert(id).second) {
      return MediaResult<IdSet>::failure(make_media_error(
          MediaErrorCode::duplicate_id, "validate_catalog",
          std::string(collection) + " contains a duplicate identifier",
          std::string(id)));
    }
  }
  return MediaResult<IdSet>::success(std::move(ids));
}

[[nodiscard]] MediaResult<void>
validate_references(const std::vector<MediaId> &references,
                    const IdSet &available, std::string_view relationship,
                    std::string_view owner) {
  IdSet seen;
  seen.reserve(references.size());
  for (const auto &reference : references) {
    if (!has_text(reference)) {
      return malformed(std::string(relationship) +
                           " contains an empty reference",
                       std::string(owner));
    }
    if (!seen.insert(reference).second) {
      return duplicate(std::string(relationship) +
                           " contains a duplicate reference",
                       reference);
    }
    if (!available.contains(reference)) {
      return missing(std::string(relationship) + " references an unknown item",
                     reference);
    }
  }
  return MediaResult<void>::success();
}

[[nodiscard]] MediaResult<void>
validate_image(const std::optional<ImageReference> &image,
               std::string_view owner) {
  if (!image) {
    return MediaResult<void>::success();
  }
  if (!has_text(image->uri)) {
    return malformed("image URI must not be empty", std::string(owner));
  }
  const bool has_width = image->pixel_width != 0;
  const bool has_height = image->pixel_height != 0;
  if (has_width != has_height) {
    return malformed("image dimensions must be both known or both unknown",
                     std::string(owner));
  }
  return MediaResult<void>::success();
}

[[nodiscard]] MediaResult<void>
validate_duration(const std::optional<MediaTime> &duration,
                  std::string_view owner) {
  if (duration && duration->count() <= 0) {
    return malformed("known duration must be positive", std::string(owner));
  }
  return MediaResult<void>::success();
}

[[nodiscard]] MediaResult<void>
validate_technical(const TechnicalMetadata &technical, std::string_view owner) {
  const bool has_width = technical.pixel_width != 0;
  const bool has_height = technical.pixel_height != 0;
  if (has_width != has_height) {
    return malformed(
        "technical video dimensions must be both known or both unknown",
        std::string(owner));
  }
  if ((technical.channel_count != 0) != (technical.sample_rate != 0)) {
    return malformed(
        "technical audio channel count and sample rate must be known together",
        std::string(owner));
  }
  for (const auto &badge : technical.badges) {
    if (!has_text(badge.label) || !has_text(badge.semantic_icon)) {
      return malformed("codec badges require a label and semantic icon",
                       std::string(owner));
    }
  }
  return MediaResult<void>::success();
}

template <typename Range, typename Predicate>
[[nodiscard]] bool contains_by(const Range &range, Predicate predicate) {
  return std::ranges::find_if(range, predicate) != range.end();
}

} // namespace

MediaResult<void> validate_catalog(const CatalogInput &input) {
  auto photo_ids = collect_ids(
      input.photos, "photos",
      [](const Photo &item) -> std::string_view { return item.id; });
  if (!photo_ids) {
    return MediaResult<void>::failure(std::move(photo_ids.error()));
  }
  auto photo_group_ids = collect_ids(
      input.photo_groups, "photo groups",
      [](const PhotoGroup &item) -> std::string_view { return item.id; });
  if (!photo_group_ids) {
    return MediaResult<void>::failure(std::move(photo_group_ids.error()));
  }
  auto artist_ids = collect_ids(
      input.artists, "artists",
      [](const MusicArtist &item) -> std::string_view { return item.id; });
  if (!artist_ids) {
    return MediaResult<void>::failure(std::move(artist_ids.error()));
  }
  auto album_ids = collect_ids(
      input.albums, "albums",
      [](const MusicAlbum &item) -> std::string_view { return item.id; });
  if (!album_ids) {
    return MediaResult<void>::failure(std::move(album_ids.error()));
  }
  auto track_ids = collect_ids(
      input.tracks, "tracks",
      [](const MusicTrack &item) -> std::string_view { return item.id; });
  if (!track_ids) {
    return MediaResult<void>::failure(std::move(track_ids.error()));
  }
  auto playlist_ids = collect_ids(
      input.playlists, "playlists",
      [](const MusicPlaylist &item) -> std::string_view { return item.id; });
  if (!playlist_ids) {
    return MediaResult<void>::failure(std::move(playlist_ids.error()));
  }
  auto video_ids = collect_ids(
      input.videos, "videos",
      [](const Video &item) -> std::string_view { return item.id; });
  if (!video_ids) {
    return MediaResult<void>::failure(std::move(video_ids.error()));
  }

  for (const auto &photo : input.photos) {
    if (!has_text(photo.title) || !has_text(photo.source_uri)) {
      return malformed("photo requires a title and source URI", photo.id);
    }
    if (photo.orientation_degrees != 0 && photo.orientation_degrees != 90 &&
        photo.orientation_degrees != 180 && photo.orientation_degrees != 270) {
      return malformed("photo orientation must be 0, 90, 180, or 270 degrees",
                       photo.id);
    }
    if ((photo.pixel_width != 0) != (photo.pixel_height != 0)) {
      return malformed("photo dimensions must be both known or both unknown",
                       photo.id);
    }
    if (auto valid = validate_image(photo.thumbnail, photo.id); !valid) {
      return valid;
    }
    if (auto valid =
            validate_references(photo.group_ids, photo_group_ids.value(),
                                "photo group membership", photo.id);
        !valid) {
      return valid;
    }
  }

  for (const auto &group : input.photo_groups) {
    if (!has_text(group.title)) {
      return malformed("photo group requires a title", group.id);
    }
    if (auto valid = validate_references(group.photo_ids, photo_ids.value(),
                                         "photo group contents", group.id);
        !valid) {
      return valid;
    }
    if (group.cover_photo_id &&
        std::ranges::find(group.photo_ids, *group.cover_photo_id) ==
            group.photo_ids.end()) {
      return missing("photo group cover must also be a group member",
                     *group.cover_photo_id);
    }
  }

  for (const auto &artist : input.artists) {
    if (!has_text(artist.name)) {
      return malformed("artist requires a name", artist.id);
    }
    if (auto valid = validate_references(artist.album_ids, album_ids.value(),
                                         "artist albums", artist.id);
        !valid) {
      return valid;
    }
    if (auto valid = validate_image(artist.portrait, artist.id); !valid) {
      return valid;
    }
  }

  for (const auto &album : input.albums) {
    if (!has_text(album.title)) {
      return malformed("album requires a title", album.id);
    }
    if (album.release_year &&
        (*album.release_year < 1 || *album.release_year > 9999)) {
      return malformed("album release year is outside the supported range",
                       album.id);
    }
    if (auto valid = validate_references(album.artist_ids, artist_ids.value(),
                                         "album artists", album.id);
        !valid) {
      return valid;
    }
    if (auto valid = validate_references(album.track_ids, track_ids.value(),
                                         "album tracks", album.id);
        !valid) {
      return valid;
    }
    if (auto valid = validate_image(album.artwork, album.id); !valid) {
      return valid;
    }
    for (const auto &track_id : album.track_ids) {
      const auto track =
          std::ranges::find_if(input.tracks, [&](const MusicTrack &candidate) {
            return candidate.id == track_id;
          });
      if (track == input.tracks.end() || !track->album_id ||
          *track->album_id != album.id) {
        return missing("album track does not link back to its album", track_id);
      }
    }
  }

  for (const auto &track : input.tracks) {
    if (!has_text(track.title) || !has_text(track.source_uri)) {
      return malformed("music track requires a title and source URI", track.id);
    }
    if (track.disc_number == 0) {
      return malformed("music track disc number must be at least one",
                       track.id);
    }
    if (auto valid = validate_duration(track.duration, track.id); !valid) {
      return valid;
    }
    if (auto valid = validate_references(track.artist_ids, artist_ids.value(),
                                         "track artists", track.id);
        !valid) {
      return valid;
    }
    if (track.album_id && !album_ids.value().contains(*track.album_id)) {
      return missing("music track references an unknown album",
                     *track.album_id);
    }
    if (auto valid = validate_image(track.artwork, track.id); !valid) {
      return valid;
    }
    if (auto valid = validate_technical(track.technical, track.id); !valid) {
      return valid;
    }
  }

  for (const auto &playlist : input.playlists) {
    if (!has_text(playlist.title)) {
      return malformed("playlist requires a title", playlist.id);
    }
    if (auto valid = validate_references(playlist.track_ids, track_ids.value(),
                                         "playlist tracks", playlist.id);
        !valid) {
      return valid;
    }
    if (auto valid = validate_image(playlist.artwork, playlist.id); !valid) {
      return valid;
    }
  }

  for (const auto &video : input.videos) {
    if (!has_text(video.title) || !has_text(video.source_uri)) {
      return malformed("video requires a title and source URI", video.id);
    }
    if (auto valid = validate_duration(video.duration, video.id); !valid) {
      return valid;
    }
    if (auto valid = validate_image(video.poster, video.id); !valid) {
      return valid;
    }
    if (auto valid = validate_technical(video.technical, video.id); !valid) {
      return valid;
    }

    IdSet chapter_ids;
    MediaTime previous_chapter{};
    bool first_chapter = true;
    for (const auto &chapter : video.chapters) {
      if (!has_text(chapter.id) || !has_text(chapter.title)) {
        return malformed("video chapter requires an identifier and title",
                         video.id);
      }
      if (!chapter_ids.insert(chapter.id).second) {
        return duplicate("video contains duplicate chapter identifiers",
                         chapter.id);
      }
      if (chapter.start.count() < 0 ||
          (!first_chapter && chapter.start <= previous_chapter)) {
        return malformed(
            "video chapters must have non-negative, strictly increasing times",
            chapter.id);
      }
      if (video.duration && chapter.start >= *video.duration) {
        return malformed("video chapter starts outside the title duration",
                         chapter.id);
      }
      if (auto valid = validate_image(chapter.thumbnail, chapter.id); !valid) {
        return valid;
      }
      previous_chapter = chapter.start;
      first_chapter = false;
    }

    IdSet subtitle_ids;
    std::size_t default_subtitles = 0;
    for (const auto &subtitle : video.subtitle_tracks) {
      if (!has_text(subtitle.id) || !has_text(subtitle.label)) {
        return malformed("subtitle track requires an identifier and label",
                         video.id);
      }
      if (!subtitle_ids.insert(subtitle.id).second) {
        return duplicate("video contains duplicate subtitle identifiers",
                         subtitle.id);
      }
      const bool external = subtitle.external_uri.has_value();
      if (external && !has_text(*subtitle.external_uri)) {
        return malformed("external subtitle URI must not be empty",
                         subtitle.id);
      }
      if (!external && !has_text(subtitle.backend_stream_key)) {
        return malformed(
            "embedded subtitle requires an opaque backend stream key",
            subtitle.id);
      }
      default_subtitles += subtitle.is_default ? 1U : 0U;
    }
    if (default_subtitles > 1) {
      return malformed("video has more than one default subtitle track",
                       video.id);
    }

    IdSet audio_ids;
    std::size_t default_audio = 0;
    for (const auto &audio : video.audio_tracks) {
      if (!has_text(audio.id) || !has_text(audio.label)) {
        return malformed("audio track requires an identifier and label",
                         video.id);
      }
      if (!audio_ids.insert(audio.id).second) {
        return duplicate("video contains duplicate audio identifiers",
                         audio.id);
      }
      const bool external = audio.external_uri.has_value();
      if (external && !has_text(*audio.external_uri)) {
        return malformed("external audio URI must not be empty", audio.id);
      }
      if (!external && !has_text(audio.backend_stream_key)) {
        return malformed("embedded audio requires an opaque backend stream key",
                         audio.id);
      }
      default_audio += audio.is_default ? 1U : 0U;
    }
    if (default_audio > 1) {
      return malformed("video has more than one default audio track", video.id);
    }

    if (video.resume) {
      if (video.resume->position.count() < 0) {
        return malformed("video resume position must be non-negative",
                         video.id);
      }
      if (video.duration && video.resume->position >= *video.duration) {
        return malformed("video resume position must precede the title end",
                         video.id);
      }
    }
  }

  // Validate the reverse side of authored relationships. A catalog with
  // asymmetric membership would otherwise render differently depending on
  // whether the browser entered through an artist, album, or group.
  for (const auto &photo : input.photos) {
    for (const auto &group_id : photo.group_ids) {
      if (!contains_by(input.photo_groups, [&](const PhotoGroup &group) {
            return group.id == group_id &&
                   std::ranges::find(group.photo_ids, photo.id) !=
                       group.photo_ids.end();
          })) {
        return missing("photo group membership is not reciprocal", photo.id);
      }
    }
  }
  for (const auto &group : input.photo_groups) {
    for (const auto &photo_id : group.photo_ids) {
      if (!contains_by(input.photos, [&](const Photo &photo) {
            return photo.id == photo_id &&
                   std::ranges::find(photo.group_ids, group.id) !=
                       photo.group_ids.end();
          })) {
        return missing("photo group contents are not reciprocal", group.id);
      }
    }
  }
  for (const auto &artist : input.artists) {
    for (const auto &album_id : artist.album_ids) {
      if (!contains_by(input.albums, [&](const MusicAlbum &album) {
            return album.id == album_id &&
                   std::ranges::find(album.artist_ids, artist.id) !=
                       album.artist_ids.end();
          })) {
        return missing("artist album membership is not reciprocal", artist.id);
      }
    }
  }
  for (const auto &album : input.albums) {
    for (const auto &artist_id : album.artist_ids) {
      if (!contains_by(input.artists, [&](const MusicArtist &artist) {
            return artist.id == artist_id &&
                   std::ranges::find(artist.album_ids, album.id) !=
                       artist.album_ids.end();
          })) {
        return missing("album artist membership is not reciprocal", album.id);
      }
    }
  }
  for (const auto &track : input.tracks) {
    if (track.album_id &&
        !contains_by(input.albums, [&](const MusicAlbum &album) {
          return album.id == *track.album_id &&
                 std::ranges::find(album.track_ids, track.id) !=
                     album.track_ids.end();
        })) {
      return missing("track album membership is not reciprocal", track.id);
    }
  }

  return MediaResult<void>::success();
}

MediaResult<Catalog> Catalog::create(CatalogInput input) {
  auto valid = validate_catalog(input);
  if (!valid) {
    return MediaResult<Catalog>::failure(std::move(valid.error()));
  }
  return MediaResult<Catalog>::success(Catalog(std::move(input)));
}

std::span<const Photo> Catalog::photos() const noexcept {
  return input_.photos;
}
std::span<const PhotoGroup> Catalog::photo_groups() const noexcept {
  return input_.photo_groups;
}
std::span<const MusicArtist> Catalog::artists() const noexcept {
  return input_.artists;
}
std::span<const MusicAlbum> Catalog::albums() const noexcept {
  return input_.albums;
}
std::span<const MusicTrack> Catalog::tracks() const noexcept {
  return input_.tracks;
}
std::span<const MusicPlaylist> Catalog::playlists() const noexcept {
  return input_.playlists;
}
std::span<const Video> Catalog::videos() const noexcept {
  return input_.videos;
}

const Photo *Catalog::find_photo(std::string_view id) const noexcept {
  const auto found = std::ranges::find(input_.photos, id, &Photo::id);
  return found == input_.photos.end() ? nullptr : &*found;
}

const MusicTrack *Catalog::find_track(std::string_view id) const noexcept {
  const auto found = std::ranges::find(input_.tracks, id, &MusicTrack::id);
  return found == input_.tracks.end() ? nullptr : &*found;
}

const Video *Catalog::find_video(std::string_view id) const noexcept {
  const auto found = std::ranges::find(input_.videos, id, &Video::id);
  return found == input_.videos.end() ? nullptr : &*found;
}

} // namespace openxmb::media
