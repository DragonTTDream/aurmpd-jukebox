import md5 from 'blueimp-md5'
import {t} from './i18n'

// Every request is aborted after this many milliseconds so a dead/hanging
// server can never leave a loader spinning forever.
const REQUEST_TIMEOUT = 10000;

/**
* Subsonic API client.
*
* Exposes methods to make requests to Subsonic API endpoints, given the
* configuration provided at initialisation time.
*
* In addition to whatever input the API methods require, success and failure
* callbacks may be provided to consume output. For example:
*
* subsonic.ping({
*   success: function(response) {
*     // use response
*   },
*   failure: function(status, message) {
*     // ...
*   }
* })
*
* Every method always ends in a catch(), so a failure (bad HTTP status, a
* non-JSON body, a timeout, ...) always reaches the error callback instead of
* silently dropping the promise chain.
*/
export default class Subsonic {

	constructor(url, user, token, salt, version, appName) {
		this.url = url.endsWith('/') ? url.substring(0, url.length - 1) : url.trim();
		this.user = user;
		this.token = token;
		this.salt = salt;
		this.version = version;
		this.appName = appName;
	}

	static createToken(password, salt) {
		return md5(password + salt);
	}

	getUrl(func, params) {
		var result = this.url + "/rest/" + func + ".view?";
		var _params = {
			u: this.user,
			t: this.token,
			s: this.salt,
			v: this.version,
			c: this.appName,
			f: "json"
		};

		Object.keys(_params).forEach(function(k) {
			result += k + "=" + _params[k] + "&";
		});

		Object.keys(params).forEach(function(k) {
			if (Array.isArray(params[k])) {
				params[k].forEach(function(v) {
					result += k + "=" + v + "&";
				});
			} else {
				result += k + "=" + params[k] + "&";
			}
		});

		return result;
	}

	/**
	* Fetch a URL and resolve with the parsed JSON body.
	*
	* Rejects with an Error for: network failure, non-2xx status, invalid JSON,
	* or a request that exceeded REQUEST_TIMEOUT.
	*/
	fetchJson(url, options) {
		var opts = Object.assign({mode: 'cors'}, options || {});
		var controller = (typeof AbortController !== 'undefined') ? new AbortController() : null;
		var timer = null;

		if (controller) {
			opts.signal = controller.signal;
			timer = setTimeout(function() {
				controller.abort();
			}, REQUEST_TIMEOUT);
		}

		function clearTimer() {
			if (timer) {
				clearTimeout(timer);
				timer = null;
			}
		}

		return fetch(url, opts).then(function(response) {
			clearTimer();
			if (!response.ok) {
				throw new Error(t('errors.httpStatus', {status: response.status}));
			}
			return response.json();
		}, function(error) {
			clearTimer();
			if (controller && controller.signal.aborted) {
				throw new Error(t('errors.timeout', {seconds: REQUEST_TIMEOUT / 1000}));
			}
			throw error;
		});
	}

	responseError(data, fallback) {
		var error = data && data['subsonic-response'] ? data['subsonic-response'].error : null;
		return new Error((error && error.message) ? error.message : fallback);
	}

	ping(params) {
		this.fetchJson(this.getUrl('ping', {}), {
			cache: 'no-cache'
		})
		.then(function(data) {
			params.success(data['subsonic-response']);
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getArtists(params) {
		this.fetchJson(this.getUrl('getArtists', {}))
		.then(function(data) {
			var allArtists = [];

			// get artists from their letter-based groups into a flat collection
			data['subsonic-response'].artists.index.map(function(letter) {
				letter.artist.map(function(artist) {
					allArtists.push(artist);
				});
			});

			// sort artists ignoring the 'ignored articles', such as 'The' etc
			var ignoredArticles = data['subsonic-response'].artists.ignoredArticles.split(' ');
			allArtists.sort(function(a, b) {
				var at = a.name;
				var bt = b.name;
				for (var i = ignoredArticles.length - 1; i >= 0; i--) {
					if (at.indexOf(ignoredArticles[i] + ' ') == 0) at = at.replace(ignoredArticles[i] + ' ', '');
					if (bt.indexOf(ignoredArticles[i] + ' ') == 0) bt = bt.replace(ignoredArticles[i] + ' ', '');
				};
				return at.localeCompare(bt);
			});

			params.success({artists: allArtists});
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getArtist(params) {
		this.fetchJson(this.getUrl('getArtist', {id: params.id}))
		.then(function(data) {
			var albums = data['subsonic-response'].artist.album;

			if (albums.length > 1) {
				albums.sort(function(a, b) {
					return (a.year || 0) - (b.year || 0);
				});
			}

			params.success({albums: albums});
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getAlbum(params) {
		this.fetchJson(this.getUrl('getAlbum', {id: params.id}))
		.then(function(data) {
			var album = data['subsonic-response'].album;
			album.song.sort(function(a, b) {
				return a.discNumber && b.discNumber
				? ((a.discNumber*1000) + a.track) - ((b.discNumber*1000) + b.track)
				: a.track - b.track;
			});
			params.success({album: album});
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getPlaylists(params) {
		this.fetchJson(this.getUrl('getPlaylists', {}))
		.then(function(data) {
			params.success({playlists: data['subsonic-response'].playlists.playlist});
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getPlaylist(params) {
		this.fetchJson(this.getUrl('getPlaylist', {id: params.id}))
		.then(function(data) {
			params.success({playlist: data['subsonic-response'].playlist});
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	createPlaylist(params) {
		this.fetchJson(this.getUrl('createPlaylist', {name: params.name, songId: params.tracks}))
		.then(function(data) {
			if (data['subsonic-response'].status == "ok") {
				params.success();
			} else {
				throw this.responseError(data, "Failed to create playlist");
			}
		}.bind(this))
		.catch(function(error) {
			params.error(error);
		});
	}

	updatePlaylist(params) {
		var options = {playlistId: params.id};
		if (params.name) options.name = params.name;
		if (params.comment) options.comment = params.comment;
		if (params.add) options.songIdToAdd = params.add;
		if (params.remove) options.songIndexToRemove = params.remove;

		this.fetchJson(this.getUrl('updatePlaylist', options))
		.then(function(data) {
			if (data['subsonic-response'].status == "ok") {
				params.success();
			} else {
				throw this.responseError(data, "Failed to update playlist");
			}
		}.bind(this))
		.catch(function(error) {
			params.error(error);
		});
	}

	deletePlaylist(params) {
		this.fetchJson(this.getUrl('deletePlaylist', {id: params.id}))
		.then(function(data) {
			if (data['subsonic-response'].status == "ok") {
				params.success();
			} else {
				throw this.responseError(data, "Failed to delete playlist");
			}
		}.bind(this))
		.catch(function(error) {
			params.error(error);
		});
	}

	search(params) {
		this.fetchJson(this.getUrl('search3', {query: params.query, songCount: params.songCount}))
		.then(function(data) {
			params.success(data['subsonic-response'].searchResult3);
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	scrobble(params) {
		this.fetchJson(this.getUrl('scrobble', {id: params.id}))
		.then(function(data) {
			params.success();
		})
		.catch(function(error) {
			params.error(error);
		});
	}

	getStreamUrl(params) {
		return this.getUrl('stream', {
			id: params.id,
			//format: params.format ? params.format : 'mp3',
			//maxBitRate: params.bitrate ? params.bitrate : 0
		});
	}

}
