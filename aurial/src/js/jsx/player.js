import {h, Component} from 'preact';
import AudioPlayer from '../audioplayer'
import {SecondsToTime, ArrayShuffle} from '../util'
import {CoverArt} from './common' 
import {Messages} from './app'
import {t} from '../i18n'


// socket 与带队列的发送函数统一在 ../mpdws 中管理（连上之前会排队，不会抛异常）
import {socket, sendCommand} from '../mpdws';
export {socket};
const fetch = require('node-fetch');

export default class Player extends Component {
	noImage = 'css/aurial_200.png';

	static defaultProps = {
		trackBuffer: false
	}

	player = null;
	queue = []; // the queue we use internally for jumping between tracks, shuffling, etc
	state = {
		mpdstate:null,
		volume: 1.0,
		playing:null,
		album: null
	}

	constructor(props, context) {
		super(props, context);
		props.events.subscribe({
			subscriber: this,
			event: ["playerToggle", "playerStop", "playerNext", "playerPrevious", "playerEnqueue", "playerVolume","songchange","playtrack","browserSelected"]
		});

	}

	componentWillUpdate(nextProps, nextState) {
	}
	componentDidMount() {
        // 监听连接建立事件
        socket.addEventListener('open', (event) => {
			console.log('ws:Connected to the server');
			//sendCommand('Hello, server!');
		});
		// 监听接收到消息事件
		socket.addEventListener('message', (event) => {
			console.log(`ws:Received message from server: ${event.data}`);//for debug
			var mpdrespond = null;
			try {
				mpdrespond = JSON.parse(event.data);
			} catch (error) {
				// 处理解析错误
				console.error("JSON解析失败:", error);
			}
			if (mpdrespond == null) return;
			if (mpdrespond.type == 'state'){
				this.props.events.publish({event: "mpdstatus",data:mpdrespond.data});
				this.setState({mpdstate:mpdrespond});
			}else if(mpdrespond.type == 'update_queue'){
				console.log('queue'+JSON.stringify(mpdrespond.data));
				this.props.events.publish({event: "playerEnqueued"});
			}else if(mpdrespond.type =='song_change'){
				//暂时没用，因为在只启动浏览器情况下不会触发song_change，当前播放队列就无法显示,而是通过tracklist记录播放歌曲id，发送songchange来实现改变歌曲
			}else{
				//其余消息(playlists/playlist/error等)通过事件总线广播，供本地歌单等组件消费
				this.props.events.publish({event: "mpdMessage", data: mpdrespond});
			}
		});
		// 监听连接关闭事件
		socket.addEventListener('close', (event) => {
			console.log('ws:Connection closed');
		});

		// 监听错误事件
		socket.addEventListener('error', (event) => {
			console.error('ws:WebSocket error:', event);
		});
		
	}
	componentWillUnmount() {
		// 组件卸载时清理事件监听器
		socket.removeEventListener('open', () => {});
		socket.removeEventListener('message', () => {});
		socket.removeEventListener('close', () => {});
		socket.removeEventListener('error', () => {});
    }
	// 入队/换队完成后给出可见反馈（成功/失败），不静默
	enqueueResult(action, tracks) {
		var count = tracks.length;
		var text = null;
		if (action === 'REPLACE') text = count > 0 ? t('player.playingCount', {count: count}) : t('player.queueCleared');
		else if (action === 'ADD') text = t('player.addedCount', {count: count});
		else if (action === 'ADDPLAY') text = t('player.addedPlayingCount', {count: count});
		if (text != null) Messages.message(this.props.events, text, "success", "checkmark");
	}

	receive(event) {
		switch (event.event) {

			case "playerToggle": this.togglePlay(); break;
			case "playerStop": this.stop(); break;
			case "playerNext": this.next(); break;
			case "playerPrevious": this.previous(); break;
			case "playerEnqueue": this.enqueue(event.data.action, event.data.tracks); break;
			case "playerVolume": this.volume(event.data); break;
			case "songchange":this.setState({playing:event.data});break;
			case "playtrack":sendCommand('MPD_API_PLAY_TRACK,' + event.data.queue_sid); break;
			case "browserSelected": this.setState({album: event.data.tracks}); break;
		}
	}

	next() {
		sendCommand('MPD_API_SET_NEXT');
	}

	previous() {
		sendCommand('MPD_API_SET_PREV');
	}

	nextTrack() {
		var next = null;
		if (this.queue.length > 0) {
			var idx = this.state.playing == null ? 0 : Math.max(0, this.queue.indexOf(this.state.playing));

			if (idx < this.queue.length - 1) {
				idx++;
			} else {
				idx = 0;
			}

			next = this.queue[idx];
		}

		return next;
	}

	previousTrack() {
		var previous = null;
		if (this.queue.length > 0) {
			var idx = this.state.playing == null ? 0 : Math.max(0, this.queue.indexOf(this.state.playing));

			if (idx > 0) idx--;
			else idx = this.queue.length - 1;

			previous = this.queue[idx];
		}

		return previous;
	}

	togglePlay() {
		// mpdstate 在首条 state 广播到达前是 null，直接取 .data 会抛异常导致按钮失灵
		if (this.state.mpdstate && this.state.mpdstate.data && this.state.mpdstate.data.state == 2)
			sendCommand('MPD_API_SET_PAUSE');
		else
			sendCommand('MPD_API_SET_PLAY');
	}

	stop() {
		sendCommand('MPD_API_SET_STOP');
	}

	volume(volume) {
		this.setState({volume: volume});
		var volume = volume*100;
		if(volume >= 99)
			volume = 100;
		if(volume <= 1)
			volume = 0;
		sendCommand('MPD_API_SET_VOLUME,'+Math.floor(volume).toString()+' ')
	}

	enqueue(action, tracks) {
		//track添加至mpd
		if(action === 'REPLACE'){
			for(let i =0;i<tracks.length;i++){
				if (!('url' in tracks[i]))
					tracks[i].url=this.props.subsonic.getStreamUrl(tracks[i]);
			}			
			fetch("/api/queue/replace/play",{ //replace the queue and play
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'Accept': 'application/json',
				},
				body: JSON.stringify(tracks)
			})
			.then(response =>{
				if (!response.ok) {
					throw new Error(t('errors.httpFailed', {status: response.status}));
				}
				return response.json();//注意：返回的是JavaScript 对象
			})
			.then((data) => {
				this.enqueueResult('REPLACE', tracks);
			})
			.catch(error => {
				console.error('request error:', error.message);
				Messages.message(this.props.events, t('player.queueUpdateFailed', {error: error.message}), "error", "warning sign");
			});
		}

		if(action === 'ADD'){
			for(let i =0;i<tracks.length;i++){
				if (!('url' in tracks[i]))
					tracks[i].url=this.props.subsonic.getStreamUrl(tracks[i]);
			}			
			fetch("/api/queue/add/",{ //add tu queue 
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'Accept': 'application/json',
				},
				body: JSON.stringify(tracks)
			})
			.then(response =>{
				if (!response.ok) {
					throw new Error(t('errors.httpFailed', {status: response.status}));
				}
				return response.json();//注意：返回的是JavaScript 对象
			})
			.then((data) => {
				this.enqueueResult('ADD', tracks);
			})
			.catch(error => {
				console.error('request error:', error.message);
				Messages.message(this.props.events, t('player.queueUpdateFailed', {error: error.message}), "error", "warning sign");
			});
		}

		if(action === 'ADDPLAY'){ // add to queue and play
			for(let i =0;i<tracks.length;i++){
				if (!('url' in tracks[i]))
					tracks[i].url=this.props.subsonic.getStreamUrl(tracks[i]);
			}			
			fetch("/api/queue/add/play",{
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'Accept': 'application/json',
				},
				body: JSON.stringify(tracks)
			})
			.then(response =>{
				if (!response.ok) {
					throw new Error(t('errors.httpFailed', {status: response.status}));
				}
				return response.json();//注意：返回的是JavaScript 对象
			})
			.then((data) => {
				this.enqueueResult('ADDPLAY', tracks);
			})
			.catch(error => {
				console.error('request error:', error.message);
				Messages.message(this.props.events, t('player.queueUpdateFailed', {error: error.message}), "error", "warning sign");
			});
		}

		if(action === 'DEL'){ // remove a track from queue
			fetch("/api/queue/del",{
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'Accept': 'application/json',
				},
				body: JSON.stringify(tracks)
			})
			.then(response =>{
				if (!response.ok) {
					throw new Error(t('errors.httpFailed', {status: response.status}));
				}
				return response.json();//注意：返回的是JavaScript 对象
			})
			.then((data) => {
				//实际不用返回tracks
			})
			.catch(error => {
				console.error('request error:', error.message);
				Messages.message(this.props.events, t('player.queueUpdateFailed', {error: error.message}), "error", "warning sign");
			});
		}
	}

	render() {
		var nowPlaying = t('player.nothingPlaying');
		var coverArt = <img src={this.noImage} />;

		if (this.state.playing != null) {
			coverArt = <CoverArt subsonic={this.props.subsonic} id={this.state.playing.coverArt} size={80} events={this.props.events} />;
		}

		return (
			<div className="ui basic segment player">
				<div className="ui items">
					<div className="ui item">
						<div className="ui tiny image">
							{coverArt}
						</div>
						<div className="content">
							<div className="header">
								<PlayerPlayingTitle events={this.props.events} playing={this.state.playing} />
							</div>
							<div className="meta">
								<PlayerPlayingInfo events={this.props.events} playing={this.state.playing} />
							</div>
							<div className="description">
								<table><tbody>
									<tr>
										<td className="controls">
											<div className="ui black icon buttons">
												<PlayerPriorButton key="prior" events={this.props.events} />
												<PlayerPlayToggleButton key="play" events={this.props.events} />
												<PlayerStopButton key="stop" events={this.props.events} />
												<PlayerNextButton key="next" events={this.props.events} />
												<PlayerShuffleButton key="shuffle" events={this.props.events} />
												<PlayerModeButton key="repeat" events={this.props.events} command="MPD_API_TOGGLE_REPEAT" param="repeat" icon="repeat" title={t('player.repeatQueue')} />
												<PlayerModeButton key="single" events={this.props.events} command="MPD_API_TOGGLE_SINGLE" param="single" icon="record" title={t('player.repeatSingle')} />
												<PlayerModeButton key="consume" events={this.props.events} command="MPD_API_TOGGLE_CONSUME" param="consume" icon="eraser" title={t('player.consume')} />
												<PlayerPositionDisplay key="time" events={this.props.events} playing={this.state.playing} />
											</div>
										</td>
										<td className="progress">
											<PlayerProgress key="progress" events={this.props.events} playing={this.state.playing}/>
										</td>
										<td className="volume">
											<PlayerVolume key="volume" events={this.props.events} volume={this.state.volume} />
										</td>
									</tr>
								</tbody></table>
							</div>
						</div>
					</div>
				</div>
			</div>
		);
	}
}

class PlayerPlayingTitle extends Component {
	render() {
		return (
			<span>
				{this.props.playing == null ? t('player.nothingPlaying') : this.props.playing.title}
			</span>
		);
	}
}

class PlayerPlayingInfo extends Component {
	render() {
		var album = t('player.nothingPlaying');
		if (this.props.playing != null) {
			album = this.props.playing.artist + " - " + this.props.playing.album;
			if (this.props.playing.date) album += " (" + this.props.playing.date + ")";
		}

		return (
			<span>
				{album}
			</span>
		);
	}
}

class PlayerPositionDisplay extends Component {
	state = {
		duration: 0,
		position: 0
	}

	constructor(props, context) {
		super(props, context);
		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.setState({duration: event.data.elapsedTime, position: event.data.totalTime});break;
		}
	}

	render() {
		//有时候status的totalTime为0，所以优先playing的时间
		var totalTime = this.props.playing == null ? this.state.position : this.props.playing.duration;
		return (
			<div className="ui disabled labeled icon button">
				<i className="clock icon"></i>
				{SecondsToTime(totalTime)}/{SecondsToTime(this.state.duration )}
			</div>
		);
	}
}

/**
* 可交互进度条：点击跳转，按住拖动本地预览、松手提交一次 seek。
*
* 用法：
*   - 目标秒数 = 比例 * totalTime；命令 MPD_API_SET_SEEK,<songid>,<pos>
*   - songid 取服务端广播的 state.currentsongid；为 -1（没有当前歌曲）或者
*     总时长未知时禁用交互，只显示进度并给出视觉提示。
*   - Pointer Events + setPointerCapture 同时覆盖鼠标与触摸；拖动期间只在本地
*     更新预览位置（不逐像素发命令），避免把 mpd 打爆。
*/
class PlayerProgress extends Component {
	state = {
		playerProgress: 0,
		loadingProgress: 0,
		totalTime: 0,
		elapsed: 0,
		songId: -1,
		preview: null,
		dragging: false
	}

	constructor(props, context) {
		super(props, context);
		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});

		this.onPointerDown = this.onPointerDown.bind(this);
		this.onPointerMove = this.onPointerMove.bind(this);
		this.onPointerUp = this.onPointerUp.bind(this);
		this.onPointerCancel = this.onPointerCancel.bind(this);
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus": this.mpdstatus(event.data); break;
		}
	}

	//有时候status的totalTime为0，所以优先playing的时间
	totalTime(mpds) {
		var fromStatus = mpds && mpds.totalTime ? mpds.totalTime : 0;
		if (fromStatus > 0) return fromStatus;
		return (this.props.playing && this.props.playing.duration) ? this.props.playing.duration : 0;
	}

	seekable() {
		return this.state.songId >= 0 && this.state.totalTime > 0;
	}

	mpdstatus(mpds) {
		var totalTime = this.totalTime(mpds);
		var elapsed = mpds.elapsedTime || 0;
		var percent = totalTime > 0 ? Math.min(100, (elapsed / totalTime) * 100) : 0;
		var next = {totalTime: totalTime, elapsed: elapsed, playerProgress: percent};
		// songid 变化（换歌）时不要沿用上一首的 id
		if (mpds.currentsongid !== undefined) next.songId = mpds.currentsongid;
		// 正在拖动时不覆盖本地预览
		if (!this.drag) this.setState(next);
		else this.setState({totalTime: totalTime, songId: next.songId, playerProgress: percent});
	}

	playerLoading(playing, loaded, total) {
		var percent = (loaded / total) * 100;
		this.setState({loadingProgress: percent});
	}

	ratioFromEvent(event) {
		var el = this.rootRef;
		if (!el) return 0;
		var rect = el.getBoundingClientRect();
		if (rect.width <= 0) return 0;
		return Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width));
	}

	onPointerDown(event) {
		if (!this.seekable()) return;
		// 阻止拖拽时选中文字/触发页面滚动
		event.preventDefault();
		this.drag = true;
		this.dragSongId = this.state.songId;
		if (event.currentTarget.setPointerCapture) {
			try { event.currentTarget.setPointerCapture(event.pointerId); } catch (e) {}
		}
		this.setState({dragging: true, preview: this.ratioFromEvent(event)});
	}

	onPointerMove(event) {
		if (!this.drag) return;
		event.preventDefault();
		this.setState({preview: this.ratioFromEvent(event)});
	}

	onPointerUp(event) {
		if (!this.drag) return;
		this.drag = false;
		var ratio = this.state.preview == null ? this.ratioFromEvent(event) : this.state.preview;
		var total = this.state.totalTime;
		var pos = Math.round(ratio * total);
		if (pos < 0) pos = 0;
		if (total > 0 && pos > total) pos = total;
		// 拖动期间换了歌/歌曲已不在队列：这次的跳转已无意义，丢弃而不是发 -1
		if (this.state.songId < 0 || this.dragSongId !== this.state.songId) {
			this.setState({dragging: false, preview: null});
			return;
		}
		// 只提交一次 seek
		sendCommand('MPD_API_SET_SEEK,' + this.state.songId + ',' + pos);
		this.setState({dragging: false, preview: null, elapsed: pos, playerProgress: ratio * 100});
	}

	onPointerCancel(event) {
		// 系统取消（例如手势被接管）：放弃本次拖动，不发命令
		this.drag = false;
		this.setState({dragging: false, preview: null});
	}

	render() {
		var seekable = this.seekable();
		var percent = (this.state.dragging && this.state.preview != null) ? this.state.preview * 100 : this.state.playerProgress;
		var playerProgress = {width: percent + "%"};
		var loadingProgress = {width: this.state.loadingProgress + "%"};
		var className = "player-progress" + (seekable ? " seekable" : " disabled") + (this.state.dragging ? " dragging" : "");
		return (
			<div className={className}
				ref={(r) => { this.rootRef = r; }}
				title={seekable ? t('player.seekHint') : t('player.seekUnavailable')}
				data-seekable={seekable ? "1" : "0"}
				data-songid={this.state.songId}
				data-total={this.state.totalTime}
				data-elapsed={this.state.elapsed}
				onPointerDown={this.onPointerDown}
				onPointerMove={this.onPointerMove}
				onPointerUp={this.onPointerUp}
				onPointerCancel={this.onPointerCancel}>
				<div className="ui red progress">
					<i className="clock icon"></i>
					<div className="track bar" style={playerProgress}></div>
					<div className="loading bar" style={loadingProgress}></div>
				</div>
			</div>
		);
	}
}


/**
* 音量条：Pointer Events（鼠标 + 触摸通用），按住后用 setPointerCapture 捕获指针，
* 拖出元素边界也能继续调音量。
*/
class PlayerVolume extends Component {

	constructor(props, context) {
		super(props, context);

		this.onPointerDown = this.onPointerDown.bind(this);
		this.onPointerUp = this.onPointerUp.bind(this);
		this.onPointerMove = this.onPointerMove.bind(this);
		this.onPointerCancel = this.onPointerCancel.bind(this);
	}

	componentWillUnmount() {
	}

	onPointerDown(event) {
		this.drag = true;
		if (event.currentTarget.setPointerCapture) {
			try { event.currentTarget.setPointerCapture(event.pointerId); } catch (e) {}
		}
		this.onPointerMove(event);
	}

	onPointerUp(event) {
		this.drag = false;
	}

	onPointerCancel(event) {
		this.drag = false;
	}

	onPointerMove(event) {
		if (this.drag) {
			var el = this.rootRef || document.querySelector(".player-volume");
			if (!el) return;
			var rect = el.getBoundingClientRect();
			if (rect.width <= 0) return;
			var volume = Math.min(1.0, Math.max(0.0, (event.clientX - rect.left) / rect.width));

			this.props.events.publish({event: "playerVolume", data: volume});
		}
	}

	render() {
		var playerVolume = {width: (this.props.volume*100) + "%"};
		return (
			<div className="player-volume"
				ref={(r) => { this.rootRef = r; }}
				onPointerDown={this.onPointerDown} onPointerMove={this.onPointerMove}
				onPointerUp={this.onPointerUp} onPointerCancel={this.onPointerCancel}>
				<div className="ui blue progress">
					<i className="volume up icon"></i>
					<div className="bar" style={playerVolume}></div>
				</div>
			</div>
		);
	}
}

class PlayerPlayToggleButton extends Component {
	state = {
		paused: false,
		playing: false,
		enabled: false
	}

	constructor(props, context) {
		super(props, context);

		this.onClick = this.onClick.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.mpdstatus(event.data);break;
		}
	}

	mpdstatus(mpds){
	var playing=false;var paused=false;var enabled = false;
	if(mpds.state == 2)//play
		playing = true;
	else if(mpds.state == 3)//pause
		paused = true;
	else if(mpds.state == 1){//stop
		playing = false;
		paused = false;
	}
	if(mpds.queueLength > 0 )
		enabled = true
	this.setState({paused: paused, playing: playing, enabled: enabled});
	}

	playerStart(playing) {
		this.setState({paused: false, playing: true, enabled: true});
	}

	playerFinish(playing) {
		this.setState({paused: false, playing: false});
	}

	playerPause(playing) {
		this.setState({paused: true});
	}

	onClick() {
		this.props.events.publish({event: "playerToggle"});
	}

	render() {
		return (
			<button className={"ui icon button " + (this.state.enabled ? "" : "disabled")} onClick={this.onClick}>
				<i className={this.state.paused || !this.state.playing ? "play icon" : "pause icon"} />
			</button>
		);
	}
}

class PlayerStopButton extends Component {
	state = {
		enabled: false
	}

	constructor(props, context) {
		super(props, context);

		this.onClick = this.onClick.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.mpdstatus(event.data);break;
		}
	}

	mpdstatus(mpds){
		var enabled = true;
		if(mpds.state == 1) //stop
			enabled = false
		this.setState({enabled: enabled});
	}	


	playerFinish(playing) {
		this.setState({enabled: false});
	}

	onClick() {

		this.props.events.publish({event: "playerStop"});
	}

	render() {
		return (
			<button className={"ui icon button " + (this.state.enabled ? "" : "disabled")} onClick={this.onClick}>
				<i className="stop icon" />
			</button>
		);
	}
}

class PlayerNextButton extends Component {
	state = {
		enabled: false
	}

	constructor(props, context) {
		super(props, context);

		this.onClick = this.onClick.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: [,"mpdstatus"]
		});
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.mpdstatus(event.data);break;
		}
	}

	mpdstatus(mpds){
		var enabled = false;
		if(mpds.queueLength > 0)
			enabled = true
		this.setState({enabled: enabled});
	}	

	onClick() {
		this.props.events.publish({event: "playerNext"});
	}

	render() {
		return (
			<button className={"ui icon button " + (this.state.enabled ? "" : "disabled")} onClick={this.onClick}>
				<i className="fast forward icon" />
			</button>
		);
	}
}

class PlayerPriorButton extends Component {
	state = {
		enabled: false
	}

	constructor(props, context) {
		super(props, context);

		this.onClick = this.onClick.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}

	componentWillUnmount() {
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.mpdstatus(event.data);break;
		}
	}

	mpdstatus(mpds){
		var enabled = false;
		if(mpds.queueLength > 0)
			enabled = true
		this.setState({enabled: enabled});
	}		

	onClick() {
		this.props.events.publish({event: "playerPrevious"});
	}

	render() {
		return (
			<button className={"ui icon button " + (this.state.enabled ? "" : "disabled")} onClick={this.onClick}>
				<i className="fast backward icon" />
			</button>
		);
	}
}

class PlayerShuffleButton extends Component {
	state = {
		shuffle: false
	}

	constructor(props, context) {
		super(props, context);
		this.onClick = this.onClick.bind(this);
		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});

	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus":this.mpdstatus(event.data);break;
		}
	}	

	mpdstatus(mpds){
		var enabled = false;
		if((mpds.random == 0)&&(this.state.shuffle == true))
			this.setState({shuffle: false});
		if((mpds.random == 1)&&(this.state.shuffle == false))
			this.setState({shuffle: true});
	}		

	onClick() {
		if(this.state.shuffle == false)
			sendCommand("MPD_API_TOGGLE_RANDOM,1");
		else
			sendCommand("MPD_API_TOGGLE_RANDOM,0");
	}

	render() {
		return (
			<button className="ui icon button" onClick={this.onClick}>
				<i className={"random icon " + (this.state.shuffle ? "red" : "")} />
			</button>
		);
	}
}

/**
* Repeat / single / consume toggle buttons.
*
* The active state always follows the server broadcast ("mpdstatus", taken from
* the mpd state message), so several browsers on the same queue stay in sync.
* The value sent is the opposite of the last known server state.
*/
class PlayerModeButton extends Component {
	state = {
		active: false
	}

	constructor(props, context) {
		super(props, context);
		this.onClick = this.onClick.bind(this);
		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}

	receive(event) {
		switch (event.event) {
			case "mpdstatus": this.mpdstatus(event.data); break;
		}
	}

	mpdstatus(mpds) {
		var active = (mpds[this.props.param] == 1);
		if (active != this.state.active)
			this.setState({active: active});
	}

	onClick() {
		sendCommand(this.props.command + "," + (this.state.active ? 0 : 1));
	}

	render() {
		return (
			<button className="ui icon button" title={this.props.title} onClick={this.onClick}>
				<i className={this.props.icon + " icon " + (this.state.active ? "red" : "")} />
			</button>
		);
	}
}
