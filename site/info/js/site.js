var enable_debug = true;

var starterReady = (typeof $ !== 'undefined' && $.ready)
    ? $.ready.bind($)
    : function(callback) {
        if (document.readyState !== 'loading') {
            callback();
        } else {
            document.addEventListener('DOMContentLoaded', callback);
        }
    };

var UI = {

    smoothScrollToNamedAnchors: function() {
        document.querySelectorAll('a[href^="#"]').forEach(anchor => {
            anchor.addEventListener('click', function (e) {
                e.preventDefault();
                const target = document.querySelector(this.getAttribute('href'));
                if (target) {
                    target.scrollIntoView({
                        behavior: 'smooth',
                        block: 'start'
                    });
                }
            });
        });
    },

	enablePageTransitions: function() {
		var style = document.createElement('style');
		style.textContent = `
			::view-transition-old(root),
			::view-transition-new(root) {
			animation-duration: 0.25s;
			}

			::view-transition-old(root) {
			animation-name: fade-out;
			}

			::view-transition-new(root) {
			animation-name: fade-in;
			}

			@keyframes fade-out {
			from { opacity: 1; }
			to { opacity: 0; }
			}

			@keyframes fade-in {
			from { opacity: 0; }
			to { opacity: 1; }
			}`;
		document.body.appendChild(style);

		document.addEventListener('click', e => {
			const link = e.target.closest('a[href]');
			if (!link) return;

			e.preventDefault();

			document.startViewTransition(() => {
				window.location.href = link.href;
			});
		});
	},

    init: function() {
		//UI.enablePageTransitions();
        UI.smoothScrollToNamedAnchors();
        document.body.classList.add('loaded');
    },

}

starterReady(UI.init);
